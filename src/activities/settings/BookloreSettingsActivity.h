#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Settings submenu for Booklore server sync.
 * Fields: server URL, username, password, test connection.
 */
class BookloreSettingsActivity final : public Activity {
 public:
  explicit BookloreSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookloreSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
  void testConnection();
};
