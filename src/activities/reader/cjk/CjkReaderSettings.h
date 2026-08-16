#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

// Persisted config for the optional CJK vertical reader engine. Deliberately
// its own PersistableStore (own JSON file), not fields on CrossPointSettings
// -- see add_CJKreader.md for why: CrossPointSettings is read far outside
// the reader and is the highest-churn file in the repo for settings-related
// upstream changes, so a separate store keeps this feature's diff isolated
// the same way RecentBooksStore/OpdsServerStore/WifiCredentialStore already
// isolate theirs.
class CjkReaderSettings : public PersistableStore<CjkReaderSettings> {
 public:
  enum ReadingMode : uint8_t { VERTICAL_RL = 0, HORIZONTAL_LR = 1 };
  enum ColumnSpacing : uint8_t { SPACING_SMALL = 0, SPACING_MEDIUM = 1, SPACING_LARGE = 2 };

  // Master switch. When false, ReaderActivity always uses the standard
  // EpubReaderActivity regardless of every other field below.
  bool enabled = false;

  // Independent from SETTINGS.sdFontFamilyName: the CJK reader may want a
  // dedicated CJK family even when the standard reader uses a Latin font.
  // Empty = not set; the engine refuses to enable without one (no built-in
  // CJK font to fall back to).
  std::string fontFamilyName;
  uint8_t fontPointSize = 0;  // 0 = not set; snapped to a size the family ships once picked

  uint8_t readingMode = VERTICAL_RL;
  uint8_t columnSpacing = SPACING_MEDIUM;
  bool kinsokuEnabled = true;

  static const char* getFilePath() { return "/.crosspoint/cjk_reader_settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

 private:
  CjkReaderSettings() = default;
  ~CjkReaderSettings() = default;

  friend class PersistableStore<CjkReaderSettings>;
};

#define CJK_READER_SETTINGS CjkReaderSettings::getInstance()
