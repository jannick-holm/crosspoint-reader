#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "BookloreClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Syncs reading progress with a Booklore server.
 *
 * The caller (EpubReaderActivity) must:
 *   1. Pre-compute localProgressPercent and epub title/author while the Epub is in RAM.
 *   2. Save progress to SD and release the Epub from heap.
 *   3. Construct and replace to this activity.
 *
 * Flow: WiFi → Auth → Search book → Compare → Apply/Upload → Return to reader.
 */
class BookloreSyncActivity final : public Activity {
 public:
  explicit BookloreSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::string& epubPath, const std::string& title,
                                const std::string& author, float localProgressPercent,
                                int currentSpineIndex, int currentPage, int currentTotalPages)
      : Activity("BookloreSync", renderer, mappedInput),
        epubPath(epubPath),
        epubTitle(title),
        epubAuthor(author),
        localProgressPercent(localProgressPercent),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        currentTotalPages(currentTotalPages) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == SYNCING || state == UPLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS,
    ALREADY_SYNCED
  };

  std::string epubPath;
  std::string epubTitle;
  std::string epubAuthor;
  float localProgressPercent;
  int currentSpineIndex;
  int currentPage;
  int currentTotalPages;

  State state = WIFI_SELECTION;
  std::string statusMessage;

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()

  BookloreBookMatch bookMatch{};
  float remoteProgressPercent = 0.0f;
  CrossPointPosition remotePosition{};

  int selectedOption = 0;
  bool wifiActivated = false;

  void onWifiConnected();
  void performSync();
  void performUpload();
  void applyRemoteProgress();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
};
