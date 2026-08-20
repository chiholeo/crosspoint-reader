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
    // Zero-width bold/italic style marker (see CjkVerticalLayout.h) -- not
    // a visible character, doesn't count toward this column's row budget.
    // Style state itself is computed separately (computePageStartStyles);
    // this scan only needs to skip past these, not track them, since a
    // kinsoku-adjusted break can land before a sentinel that was inside
    // this same scan window.
    if (isStyleSentinel(cp)) continue;
    // Forced column break: end this column here (a paragraph never
    // continues mid-column) without consuming the separator -- the next
    // call's leading-skip above consumes it instead.
    if (cp == PARAGRAPH_SEPARATOR) {
      scan = before;
      break;
    }
    // Same forced-break, non-consuming treatment for an image sentinel --
    // it must never be swept up as a drawable column character (it isn't
    // one), and buildPageIndex()'s caller is the one that actually
    // consumes it, to give the image its own dedicated page. In practice
    // buildPageIndex() always intercepts an image sentinel itself before
    // calling nextColumnEnd() with a cursor pointing directly at one (see
    // its own comment), so this is a defensive backstop, not a path
    // exercised by the current caller.
    if (isImageSentinel(cp)) {
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
                                   const bool kinsokuEnabled, std::vector<int>* outPageImageIndex) {
  std::vector<size_t> pages;
  pages.push_back(0);
  if (outPageImageIndex) outPageImageIndex->clear();

  if (text.size() == 0 || metrics.rowsPerColumn <= 0 || metrics.columnsPerPage <= 0) {
    pages.push_back(text.size());
    if (outPageImageIndex) outPageImageIndex->push_back(-1);
    return pages;
  }

  size_t cursor = 0;
  const size_t end = text.size();
  int imageOrdinal = 0;

  while (cursor < end) {
    // An image sentinel right at the start of what would be this page gets
    // the page entirely to itself: consumed here directly (not via
    // nextColumnEnd(), which treats it as a non-consuming forced break --
    // see that function's own comment) and the page closes immediately,
    // without running the normal column loop at all.
    const PeekResult leading = peekCodepointAt(text, cursor);
    if (leading.consumed != 0 && isImageSentinel(leading.codepoint)) {
      cursor += leading.consumed;
      pages.push_back(cursor);
      if (outPageImageIndex) outPageImageIndex->push_back(imageOrdinal);
      imageOrdinal++;
      continue;
    }

    for (int col = 0; col < metrics.columnsPerPage && cursor < end; col++) {
      cursor = nextColumnEnd(text, cursor, metrics, kinsokuEnabled);
      // An image sentinel immediately following this column ends the page
      // early -- even short of columnsPerPage -- so the image starts a
      // fresh page of its own next, rather than sitting buried mid-page
      // where the per-page image check below couldn't find it.
      const PeekResult afterCol = peekCodepointAt(text, cursor);
      if (afterCol.consumed != 0 && isImageSentinel(afterCol.codepoint)) break;
    }
    pages.push_back(cursor);
    if (outPageImageIndex) outPageImageIndex->push_back(-1);
  }

  return pages;
}

std::vector<uint8_t> computePageStartStyles(const CjkChapterFileReader& text, const std::vector<size_t>& pageIndex) {
  std::vector<uint8_t> styles;
  if (pageIndex.size() < 2) return styles;
  styles.reserve(pageIndex.size() - 1);

  uint8_t currentStyle = 0;
  size_t scanPos = 0;
  constexpr size_t kChunkSize = 512;
  unsigned char buf[kChunkSize];
  // Bytes of a possible sentinel carried over from the tail of the previous
  // chunk, in case the 3-byte pattern straddles a chunk boundary.
  size_t carry = 0;

  for (size_t p = 0; p + 1 < pageIndex.size(); p++) {
    const size_t pageStart = pageIndex[p];
    while (scanPos < pageStart) {
      const size_t want = std::min(kChunkSize - carry, pageStart - scanPos);
      const size_t got = text.readRange(scanPos, reinterpret_cast<char*>(buf + carry), want);
      if (got == 0) break;
      const size_t total = carry + got;
      size_t k = 0;
      for (; k + 2 < total; k++) {
        if (buf[k] == 0xEE && buf[k + 1] == 0x80 && (buf[k + 2] & 0xFC) == 0x80) {
          currentStyle = buf[k + 2] & 0x03;
        }
      }
      carry = std::min<size_t>(2, total);
      for (size_t c = 0; c < carry; c++) buf[c] = buf[total - carry + c];
      scanPos += got;
    }
    styles.push_back(currentStyle);
  }
  return styles;
}

}  // namespace CjkVerticalLayout
