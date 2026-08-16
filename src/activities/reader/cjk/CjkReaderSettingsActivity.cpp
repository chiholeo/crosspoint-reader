#include "CjkReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CjkReaderSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
enum MenuItem {
  ITEM_ENABLE = 0,
  ITEM_FONT_FAMILY,
  ITEM_FONT_SIZE,
  ITEM_READING_MODE,
  ITEM_COLUMN_SPACING,
  ITEM_KINSOKU,
  ITEM_COUNT
};

const StrId menuNames[ITEM_COUNT] = {
    StrId::STR_CJK_ENABLE,     StrId::STR_CJK_FONT_FAMILY,     StrId::STR_CJK_FONT_SIZE,
    StrId::STR_CJK_READING_MODE, StrId::STR_CJK_COLUMN_SPACING, StrId::STR_CJK_KINSOKU,
};

constexpr int READING_MODE_ITEMS = 2;
const StrId readingModeNames[READING_MODE_ITEMS] = {StrId::STR_CJK_VERTICAL_RL, StrId::STR_CJK_HORIZONTAL_LR};

constexpr int SPACING_ITEMS = 3;
const StrId spacingNames[SPACING_ITEMS] = {StrId::STR_CJK_SPACING_SMALL, StrId::STR_CJK_SPACING_MEDIUM,
                                           StrId::STR_CJK_SPACING_LARGE};
}  // namespace

void CjkReaderSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  fontRegistry.discover();

  // If the previously selected family is no longer installed, clear it
  // rather than silently keep pointing at a font that can't load.
  if (!CJK_READER_SETTINGS.fontFamilyName.empty() &&
      !fontRegistry.findFamily(CJK_READER_SETTINGS.fontFamilyName)) {
    CJK_READER_SETTINGS.fontFamilyName.clear();
    CJK_READER_SETTINGS.fontPointSize = 0;
    CJK_READER_SETTINGS.saveToFile();
  }

  requestUpdate();
}

void CjkReaderSettingsActivity::onExit() { Activity::onExit(); }

void CjkReaderSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  switch (handleListTouch(selectedIndex, ITEM_COUNT, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      handleSelection();
      requestUpdate();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
}

void CjkReaderSettingsActivity::openFontFamilyPicker() {
  const auto& families = fontRegistry.getFamilies();
  std::vector<std::string> names;
  names.reserve(families.size());
  for (const auto& f : families) names.push_back(f.name);

  if (names.empty()) return;

  int currentIndex = 0;
  for (int i = 0; i < static_cast<int>(names.size()); i++) {
    if (names[i] == CJK_READER_SETTINGS.fontFamilyName) {
      currentIndex = i;
      break;
    }
  }

  optionPopup.show(StrId::STR_CJK_FONT_FAMILY, names, currentIndex, [this, families](int idx) {
    CJK_READER_SETTINGS.fontFamilyName = families[idx].name;
    // Snap the point size to something this family actually ships, the same
    // way the standard reader's font family picker does.
    const auto sizes = families[idx].availableSizes();
    if (!sizes.empty()) {
      const bool keepCurrent = std::find(sizes.begin(), sizes.end(), CJK_READER_SETTINGS.fontPointSize) !=
                               sizes.end();
      if (!keepCurrent) CJK_READER_SETTINGS.fontPointSize = sizes.front();
    } else {
      CJK_READER_SETTINGS.fontPointSize = 0;
    }
    CJK_READER_SETTINGS.saveToFile();
  });
}

void CjkReaderSettingsActivity::openFontSizePicker() {
  const auto* family = fontRegistry.findFamily(CJK_READER_SETTINGS.fontFamilyName);
  if (!family) return;
  const auto sizes = family->availableSizes();
  if (sizes.empty()) return;

  std::vector<std::string> labels;
  labels.reserve(sizes.size());
  int currentIndex = 0;
  for (size_t i = 0; i < sizes.size(); i++) {
    labels.push_back(std::to_string(sizes[i]) + "pt");
    if (sizes[i] == CJK_READER_SETTINGS.fontPointSize) currentIndex = static_cast<int>(i);
  }

  optionPopup.show(StrId::STR_CJK_FONT_SIZE, labels, currentIndex, [this, sizes](int idx) {
    CJK_READER_SETTINGS.fontPointSize = sizes[idx];
    CJK_READER_SETTINGS.saveToFile();
  });
}

void CjkReaderSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case ITEM_ENABLE:
      CJK_READER_SETTINGS.enabled = !CJK_READER_SETTINGS.enabled;
      break;
    case ITEM_FONT_FAMILY:
      openFontFamilyPicker();
      return;
    case ITEM_FONT_SIZE:
      openFontSizePicker();
      return;
    case ITEM_READING_MODE:
      optionPopup.show(StrId::STR_CJK_READING_MODE, readingModeNames, READING_MODE_ITEMS,
                       CJK_READER_SETTINGS.readingMode, [this](int idx) {
                         CJK_READER_SETTINGS.readingMode = static_cast<uint8_t>(idx);
                         CJK_READER_SETTINGS.saveToFile();
                       });
      return;
    case ITEM_COLUMN_SPACING:
      optionPopup.show(StrId::STR_CJK_COLUMN_SPACING, spacingNames, SPACING_ITEMS,
                       CJK_READER_SETTINGS.columnSpacing, [this](int idx) {
                         CJK_READER_SETTINGS.columnSpacing = static_cast<uint8_t>(idx);
                         CJK_READER_SETTINGS.saveToFile();
                       });
      return;
    case ITEM_KINSOKU:
      CJK_READER_SETTINGS.kinsokuEnabled = !CJK_READER_SETTINGS.kinsokuEnabled;
      break;
    default:
      return;
  }
  CJK_READER_SETTINGS.saveToFile();
}

void CjkReaderSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CJK_READER));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, ITEM_COUNT, selectedIndex,
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) -> std::string {
        switch (index) {
          case ITEM_ENABLE:
            return CJK_READER_SETTINGS.enabled ? tr(STR_ON) : tr(STR_OFF);
          case ITEM_FONT_FAMILY:
            return CJK_READER_SETTINGS.fontFamilyName.empty() ? tr(STR_NOT_SET) : CJK_READER_SETTINGS.fontFamilyName;
          case ITEM_FONT_SIZE:
            return CJK_READER_SETTINGS.fontPointSize == 0 ? tr(STR_NOT_SET)
                                                           : std::to_string(CJK_READER_SETTINGS.fontPointSize) + "pt";
          case ITEM_READING_MODE:
            return I18N.get(readingModeNames[CJK_READER_SETTINGS.readingMode]);
          case ITEM_COLUMN_SPACING:
            return I18N.get(spacingNames[CJK_READER_SETTINGS.columnSpacing]);
          case ITEM_KINSOKU:
            return CJK_READER_SETTINGS.kinsokuEnabled ? tr(STR_ON) : tr(STR_OFF);
          default:
            return "";
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
