#include "CjkReaderSettings.h"

void CjkReaderSettings::toJson(JsonDocument& doc) const {
  doc["enabled"] = enabled;
  doc["fontFamilyName"] = fontFamilyName;
  doc["fontPointSize"] = fontPointSize;
  doc["readingMode"] = readingMode;
  doc["columnSpacing"] = columnSpacing;
  doc["kinsokuEnabled"] = kinsokuEnabled;
}

bool CjkReaderSettings::fromJson(JsonVariantConst doc) {
  enabled = doc["enabled"] | false;
  fontFamilyName = doc["fontFamilyName"] | "";
  fontPointSize = doc["fontPointSize"] | 0;
  readingMode = doc["readingMode"] | static_cast<uint8_t>(VERTICAL_RL);
  columnSpacing = doc["columnSpacing"] | static_cast<uint8_t>(SPACING_MEDIUM);
  kinsokuEnabled = doc["kinsokuEnabled"] | true;

  if (readingMode > HORIZONTAL_LR) readingMode = VERTICAL_RL;
  if (columnSpacing > SPACING_LARGE) columnSpacing = SPACING_MEDIUM;

  return true;
}
