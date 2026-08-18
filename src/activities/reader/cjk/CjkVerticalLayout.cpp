#include "CjkVerticalLayout.h"

#include <Utf8.h>

#include <algorithm>

#include "CjkKinsoku.h"

namespace CjkVerticalLayout {

namespace {
// Reads up to 4 bytes starting at `pos` and decodes one codepoint --
// mirrors utf8NextCodepoint's NUL-terminator contract (unfilled trailing
// bytes stay zero) so a codepoint truncated at the true end of `text` is
// handled the same deterministic way as the main scan window below,
// instead of reading whatever garbage happens to be in an uninitialized
// buffer past what readRange() actually filled.
struct PeekResult {
  uint32_t codepoint;
  size_t consumed;
};

PeekResult peekCodepointAt(const CjkChapterFileReader& text, const size_t pos) {
  if (pos >= text.size()) return {0, 0};
  unsigned char buf[4] = {0, 0, 0, 0};
  text.readRange(pos, reinterpret_cast<char*>(buf), 4);
  const unsigned char* p = buf;
  const uint32_t cp = utf8NextCodepoint(&p);
  return {cp, static_cast<size_t>(p - buf)};
}
}  // namespace

std::string extractRange(const CjkChapterFileReader& text, const size_t start, const size_t end) {
  std::string result;
  if (start >= text.size() || end <= start) return result;
  const size_t clampedEnd = std::min(end, text.size());
  const size_t len = clampedEnd - start;
  result.resize(len);
  const size_t got = text.readRange(start, &result[0], len);
  if (got < len) result.resize(got);
  return result;
}

size_t nextColumnEnd(const CjkChapterFileReader& text, const size_t offset, const PageMetrics& metrics,
                     const bool kinsokuEnabled) {
  const size_t end = text.size();
  size_t cursor = offset;

  // A column never starts with a paragraph break: consume any leading
  // separator(s) here so the caller's next column starts on real text.
  while (cursor < end) {
    const PeekResult r = peekCodepointAt(text, cursor);
    if (r.consumed == 0 || r.codepoint != PARAGRAPH_SEPARATOR) break;
    cursor += r.consumed;
  }

  if (cursor >= end || metrics.rowsPerColumn <= 0) return end;

  const int scanCap = std::min(MAX_COLUMN_SCAN, metrics.rowsPerColumn + KINSOKU_LOOKAHEAD);

  // Read a bounded window from disk into a local stack buffer so the scan
  // below can use plain pointer arithmetic -- `text` is file-backed (see
  // CjkChapterText.h for why), not an in-RAM contiguous buffer. Worst case
  // scanCap codepoints at up to 4 UTF-8 bytes each, zero-initialized so a
  // codepoint truncated at the true end of `text` hits a deterministic 0
  // byte (same NUL-terminator contract utf8NextCodepoint already relies on)
  // instead of reading uninitialized stack memory.
  constexpr size_t kMaxWindowBytes = MAX_COLUMN_SCAN * 4;
  unsigned char window[kMaxWindowBytes] = {};
  const size_t windowLen = std::min(kMaxWindowBytes, end - cursor);
  text.readRange(cursor, reinterpret_cast<char*>(window), windowLen);

  const unsigned char* base = window;
  const unsigned char* wend = window + windowLen;

  uint32_t codepoints[MAX_COLUMN_SCAN];
  const unsigned char* positions[MAX_COLUMN_SCAN + 1];

  int n = 0;
  const unsigned char* scan = base;
  while (n < scanCap && scan < wend) {
    const unsigned char* before = scan;
    const uint32_t cp = utf8NextCodepoint(&scan);
    // See the PeekResult comment above: a NUL byte (real or the zero
    // padding past windowLen) makes utf8NextCodepoint() return without
    // advancing. Without this check that byte would be rescanned every
    // iteration up to scanCap, then every column for the rest of the
    // chapter -- an infinite loop in buildPageIndex's caller, since the
    // cursor would never move past it. Bail out as if this were the end of
    // the scannable text instead.
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

  if (n == 0) return end;

  const int maxColumns = std::min(metrics.rowsPerColumn, n);
  const int breakCount =
      kinsokuEnabled ? CjkKinsoku::chooseLineBreak(codepoints, static_cast<uint8_t>(n),
                                                   static_cast<uint16_t>(maxColumns))
                     : maxColumns;
  const int clamped = std::min(std::max(breakCount, 1), n);
  return cursor + static_cast<size_t>(positions[clamped] - base);
}

std::vector<size_t> buildPageIndex(const CjkChapterFileReader& text, const PageMetrics& metrics,
                                   const bool kinsokuEnabled) {
  std::vector<size_t> pages;
  pages.push_back(0);

  if (text.size() == 0 || metrics.rowsPerColumn <= 0 || metrics.columnsPerPage <= 0) {
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
