#pragma once
#include <SdCardFontRegistry.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Settings screen for the optional CJK vertical reader engine. Reached via
// its own row in Settings -> Reader (sibling of Text Settings / Manage
// Fonts / Customise Status Bar), not nested inside the Text Settings
// screen. Reads/writes CjkReaderSettings only -- never SETTINGS.
class CjkReaderSettingsActivity final : public Activity {
 public:
  explicit CjkReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CjkReaderSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  SdCardFontRegistry fontRegistry;

  int selectedIndex = 0;

  void handleSelection();
  void openFontFamilyPicker();
  void openFontSizePicker();
};
