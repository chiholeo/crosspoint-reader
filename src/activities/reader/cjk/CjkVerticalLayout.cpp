#include "CjkVerticalLayout.h"

#include <Utf8.h>

#include <algorithm>

#include "CjkKinsoku.h"

namespace CjkVerticalLayout {

size_t nextColumnEnd(const std::string& text, const size_t offset, const PageMetrics& metrics,
                     const bool kinsokuEnabled) {
  const auto* base = reinterpret_cast<const unsigned char*>(text.data());
  const unsigned char* end = base + text.size();
  const unsigned char* cursor = base + offset;

  // A column never starts with a paragraph break: consume any leading
  // separator(s) here so the caller's next column starts on real text.
  while (cursor < end) {
    const unsigned char* peek = cursor;
    const uint32_t cp = utf8NextCodepoint(&peek);
    if (peek == cursor || cp != PARAGRAPH_SEPARATOR) break;
    cursor = peek;
  }

  if (cursor >= end || metrics.rowsPerColumn <= 0) return text.size();

  const int scanCap = std::min(MAX_COLUMN_SCAN, metrics.rowsPerColumn + KINSOKU_LOOKAHEAD);
  uint32_t codepoints[MAX_COLUMN_SCAN];
  const unsigned char* positions[MAX_COLUMN_SCAN + 1];

  int n = 0;
  const unsigned char* scan = cursor;
  while (n < scanCap && scan < end) {
    const unsigned char* before = scan;
    const uint32_t cp = utf8NextCodepoint(&scan);
    // utf8NextCodepoint() treats a NUL byte as a C-string terminator and
    // does not advance past it (see Utf8.cpp). Without this check that byte
    // would be rescanned every iteration up to scanCap, then every column
    // for the rest of the chapter -- an infinite loop in buildPageIndex's
    // caller, since the cursor would never move past it. Bail out as if
    // this were the end of the scannable text instead.
    if (scan == before) break;
    // Forced column break: end this column here (a paragraph never
    // continues mid-column) without consuming the separator -- the next
    // call's leading-skip above consumes it instead.
    if (cp == PARAGRAPH_SEPARATOR) {
      scan = before;
      break;
    }
    positions[n] = before;
    codepoints[n] = cp;
    n++;
  }
  positions[n] = scan;

  if (n == 0) return text.size();

  const int maxColumns = std::min(metrics.rowsPerColumn, n);
  const int breakCount =
      kinsokuEnabled ? CjkKinsoku::chooseLineBreak(codepoints, static_cast<uint8_t>(n),
                                                   static_cast<uint16_t>(maxColumns))
                     : maxColumns;
  const int clamped = std::min(std::max(breakCount, 1), n);
  return static_cast<size_t>(positions[clamped] - base);
}

std::vector<size_t> buildPageIndex(const std::string& text, const PageMetrics& metrics, const bool kinsokuEnabled) {
  std::vector<size_t> pages;
  pages.push_back(0);

  if (text.empty() || metrics.rowsPerColumn <= 0 || metrics.columnsPerPage <= 0) {
    pages.push_back(text.size());
    return pages;
  }

  size_t cursor = 0;
  const size_t end = text.size();

  while (cursor < end) {
    for (int col = 0; col < metrics.columnsPerPage && cursor < end; col++) {
      cursor = nextColumnEnd(text, cursor, metrics, kinsokuEnabled);
    }
    pages.push_back(cursor);
  }

  return pages;
}

}  // namespace CjkVerticalLayout
