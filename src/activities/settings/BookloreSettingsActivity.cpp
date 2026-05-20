#include "BookloreSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "BookloreClient.h"
#include "BookloreCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 4;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_BOOKLORE_SERVER_URL, StrId::STR_USERNAME,
                                     StrId::STR_PASSWORD, StrId::STR_BOOKLORE_TEST_CONNECTION};
}  // namespace

void BookloreSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void BookloreSettingsActivity::onExit() { Activity::onExit(); }

void BookloreSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void BookloreSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    const std::string current = BOOKLORE_STORE.getServerUrl();
    const std::string prefill = current.empty() ? "http://" : current;
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKLORE_SERVER_URL), prefill, 128,
                                               InputType::Url),
        [](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            const std::string url = (kb.text == "http://" || kb.text == "https://") ? "" : kb.text;
            BOOKLORE_STORE.setServerUrl(url);
            BOOKLORE_STORE.saveToFile();
          }
        });
  } else if (selectedIndex == 1) {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME),
                                               BOOKLORE_STORE.getUsername(), 64, InputType::Text),
        [](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKLORE_STORE.setCredentials(kb.text, BOOKLORE_STORE.getPassword());
            BOOKLORE_STORE.saveToFile();
          }
        });
  } else if (selectedIndex == 2) {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD),
                                               BOOKLORE_STORE.getPassword(), 64, InputType::Password),
        [](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKLORE_STORE.setCredentials(BOOKLORE_STORE.getUsername(), kb.text);
            BOOKLORE_STORE.saveToFile();
          }
        });
  } else if (selectedIndex == 3) {
    testConnection();
  }
}

void BookloreSettingsActivity::testConnection() {
  if (!BOOKLORE_STORE.hasCredentials()) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               testConnection();
                             }
                           });
    return;
  }

  const auto err = BookloreClient::authenticate();

  // Show result briefly via status text re-render (next render pass picks it up)
  // The credential store retains the token state; activity re-renders on next loop tick.
  requestUpdate();

  if (err != BookloreClient::OK) {
    LOG_ERR("BLS", "Test connection failed: %s", BookloreClient::errorString(err));
  } else {
    LOG_DBG("BLS", "Test connection successful");
  }
}

void BookloreSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_BOOKLORE_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [](int index) -> std::string {
        if (index == 0) {
          const auto& url = BOOKLORE_STORE.getServerUrl();
          return url.empty() ? std::string(tr(STR_NOT_SET)) : url;
        } else if (index == 1) {
          const auto& user = BOOKLORE_STORE.getUsername();
          return user.empty() ? std::string(tr(STR_NOT_SET)) : user;
        } else if (index == 2) {
          return BOOKLORE_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == 3) {
          return BOOKLORE_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
