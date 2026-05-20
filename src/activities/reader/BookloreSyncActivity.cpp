#include "BookloreSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "BookloreCredentialStore.h"
#include "EpubReaderUtils.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BookloreSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  if (!BOOKLORE_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  wifiActivated = true;

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("BLS", "Already connected to WiFi");
    onWifiConnected();
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             onWifiConnected();
                           } else {
                             returnToReader();
                           }
                         });
}

void BookloreSyncActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void BookloreSyncActivity::onWifiConnected() {
  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_BOOKLORE_SYNCING);
  }
  requestUpdate(true);
  performSync();
}

void BookloreSyncActivity::performSync() {
  // Authenticate
  const auto authErr = BookloreClient::authenticate();
  if (authErr != BookloreClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = BookloreClient::errorString(authErr);
    }
    requestUpdate(true);
    return;
  }

  // Search for book by title+author
  auto searchErr = BookloreClient::searchBook(epubTitle, epubAuthor, bookMatch);

  if (searchErr == BookloreClient::BOOK_NOT_FOUND && !epubAuthor.empty()) {
    // Retry with title only
    LOG_DBG("BLS", "Title+author search failed, retrying with title only");
    searchErr = BookloreClient::searchBook(epubTitle, "", bookMatch);
  }

  if (searchErr != BookloreClient::OK) {
    {
      RenderLock lock(*this);
      state = (searchErr == BookloreClient::BOOK_NOT_FOUND) ? SYNC_FAILED : SYNC_FAILED;
      statusMessage = BookloreClient::errorString(searchErr);
    }
    requestUpdate(true);
    return;
  }

  remoteProgressPercent = bookMatch.progressPercent;
  LOG_DBG("BLS", "Book matched: id=%ld remote=%.1f%% local=%.1f%%",
          bookMatch.bookId, remoteProgressPercent, localProgressPercent);

  // Already in sync (within 1%)?
  if (fabsf(remoteProgressPercent - localProgressPercent) < 1.0f) {
    {
      RenderLock lock(*this);
      state = ALREADY_SYNCED;
    }
    requestUpdate(true);
    return;
  }

  if (remoteProgressPercent <= 0.0f) {
    // No remote progress yet — offer to upload local
    ensureEpubLoaded();
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
    }
    requestUpdate(true);
    return;
  }

  ensureEpubLoaded();

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    selectedOption = (localProgressPercent >= remoteProgressPercent) ? 1 : 0;
  }
  requestUpdate(true);
}

void BookloreSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  const auto err = BookloreClient::updateProgress(bookMatch, localProgressPercent);

  esp_wifi_stop();

  if (err != BookloreClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = BookloreClient::errorString(err);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void BookloreSyncActivity::ensureEpubLoaded() {
  if (epub) return;
  LOG_DBG("BLS", "Loading epub for position conversion (heap: %u)", (unsigned)ESP.getFreeHeap());
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(false, true)) {
    LOG_ERR("BLS", "Failed to load epub");
    epub.reset();
  } else {
    LOG_DBG("BLS", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void BookloreSyncActivity::saveProgressAndReturn(int spineIndex, int page) {
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void BookloreSyncActivity::applyRemoteProgress() {
  // Epub was released before TLS; reload now that TLS memory is freed.
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  // Convert remote percentage to CrossPoint spine/page using ProgressMapper.
  KOReaderPosition remoteKoPos;
  remoteKoPos.percentage = remoteProgressPercent / 100.0f;
  remoteKoPos.xpath = "";
  remotePosition = ProgressMapper::toCrossPoint(epub, remoteKoPos, currentSpineIndex, 0);

  saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
}

void BookloreSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

void BookloreSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BOOKLORE_SYNC));

  const int top = screen.y + screen.height / 2 - 40;

  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_BOOKLORE_SETUP_HINT));
    {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == ALREADY_SYNCED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_BOOKLORE_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);
    {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());
    {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_BOOKLORE_SYNC_DONE), true,
                              EpdFontFamily::BOLD);
    {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));
    {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    char remoteStr[32];
    snprintf(remoteStr, sizeof(remoteStr), "%.0f%%", remoteProgressPercent);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, contentTop + 40, tr(STR_REMOTE_LABEL),
                      true);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, contentTop + 65, remoteStr);

    char localStr[32];
    snprintf(localStr, sizeof(localStr), "%.0f%%", localProgressPercent);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, contentTop + 110, tr(STR_LOCAL_LABEL),
                      true);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, contentTop + 135, localStr);

    const int optionY = contentTop + 175;
    constexpr int optionH = 30;

    if (selectedOption == 0) renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionH);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_REMOTE),
                      selectedOption != 0);

    if (selectedOption == 1) renderer.fillRect(screen.x, optionY + optionH - 2, screen.width - 1, optionH);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionH, tr(STR_UPLOAD_LOCAL),
                      selectedOption != 1);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void BookloreSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == ALREADY_SYNCED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      performUpload();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        applyRemoteProgress();
      } else {
        performUpload();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
