#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CjkChapterFileReader.h"

// Vertical-column pagination for the CJK reader. Pure pagination math over
// an already-extracted plain-text chapter (see CjkChapterText) -- no SD,
// framebuffer, or font-object access of its own beyond reading through
// CjkChapterFileReader. Font metrics come in as plain ints the caller
// already measured, so this stays substrate-agnostic.
namespace CjkVerticalLayout {

struct PageMetrics {
  int rowsPerColumn;    // codepoints per column, from viewport height / (glyph advance + row spacing)
  int columnsPerPage;   // columns per page, from viewport width / (glyph advance + column spacing)
};

// Codepoints scanned past a column's raw capacity so CjkKinsoku::chooseLineBreak
// has room to pull the break point backward/forward. Mirrors the reference
// fork's chooseLineBreak contract (count > maxColumns lets it search).
constexpr int KINSOKU_LOOKAHEAD = 4;

// Upper bound on codepoints considered for a single column (rowsPerColumn +
// KINSOKU_LOOKAHEAD, clamped). Defends the fixed-size scan buffer in
// buildPageIndex()/nextPageOffset() against a pathologically tall viewport.
constexpr int MAX_COLUMN_SCAN = 64;

// Sentinel codepoint CjkChapterText emits at paragraph/block boundaries
// (real Unicode PARAGRAPH SEPARATOR, so it never collides with genuine book
// text). nextColumnEnd() treats it as a forced column break -- consumed,
// never rendered -- so each paragraph starts at the top of a fresh column
// instead of continuing mid-column from the previous one.
constexpr uint32_t PARAGRAPH_SEPARATOR = 0x2029;

// Private-Use Area sentinels marking a 2-bit text style state, emitted by
// CjkChapterText for <h1>-<h6> (bit 0: all six levels collapse to the same
// single "heading" level -- see the class comment there for why: one size
// tier up from body text, not six different sizes) and <b>/<strong> (bit 1).
// The two bits are independent -- bold heading text is a real, valid
// combination -- so there are 4 sentinel values, not 2. Depth counters in
// the extractor handle nesting/malformed markup; a sentinel is emitted
// whenever the combined 2-bit value changes, letting the renderer switch
// both which font id it draws with (heading bit) and which style variant
// (bold bit). Zero-width: nextColumnEnd() below skips them without counting
// toward a column's row budget, and the renderer skips them too, just
// updating its current-style state instead of drawing.
constexpr uint32_t STYLE_SENTINEL_BASE = 0xE000;
constexpr uint32_t STYLE_SENTINEL_NORMAL = STYLE_SENTINEL_BASE;
constexpr uint32_t STYLE_SENTINEL_HEADING = STYLE_SENTINEL_BASE + 1;
constexpr uint32_t STYLE_SENTINEL_BOLD = STYLE_SENTINEL_BASE + 2;
constexpr uint32_t STYLE_SENTINEL_HEADING_BOLD = STYLE_SENTINEL_BASE + 3;
constexpr uint32_t STYLE_SENTINEL_END = STYLE_SENTINEL_HEADING_BOLD;
constexpr uint8_t STYLE_BIT_HEADING = 1;
constexpr uint8_t STYLE_BIT_BOLD = 2;
inline bool isStyleSentinel(const uint32_t cp) { return cp >= STYLE_SENTINEL_BASE && cp <= STYLE_SENTINEL_END; }
inline uint8_t styleFromSentinel(const uint32_t cp) { return static_cast<uint8_t>(cp - STYLE_SENTINEL_BASE); }

// Private-Use Area sentinel marking an <img> tag's position, emitted by
// CjkChapterText (a distinct PUA point from the style-sentinel range above,
// not part of that 2-bit scheme). Zero-width like the style sentinels, but
// handled differently by pagination: buildPageIndex() below gives the image
// its own dedicated page (a full-page image break -- see
// CjkVerticalReaderActivity's renderImagePage()) rather than treating it as
// an inline character. Each occurrence corresponds, in extraction order, to
// one entry in the chapter's separately-tracked image href list (the
// sentinel itself can't carry a variable-length path).
constexpr uint32_t IMAGE_SENTINEL = 0xE010;
inline bool isImageSentinel(const uint32_t cp) { return cp == IMAGE_SENTINEL; }

// Byte offsets into `text` (UTF-8) where each page begins, plus a final
// trailing entry equal to text.size(). pages[i]..pages[i+1] is page i's
// byte range. Built once per chapter.
//
// `text` is a CjkChapterFileReader, not an in-RAM buffer: see
// CjkChapterText.h for why (holding a whole chapter's text resident --
// even split into small chunks -- competes with the zip/inflate stream's
// own ~44KB decompressor state+window for memory that on-device testing
// showed isn't reliably there). nextColumnEnd() below reads only a small
// bounded window from disk for scanning, so pagination never needs more
// than ~MAX_COLUMN_SCAN codepoints resident at a time.
//
// outPageImageIndex, if non-null, is filled with one entry per page (same
// indexing as the returned vector minus its trailing sentinel entry): -1
// for an ordinary text page, or the image's ordinal position (0-based, in
// extraction order) within the chapter's separately-tracked image href
// list for a page that's a dedicated full-page image break. Computed
// directly here rather than via a separate post-pass like
// computePageStartStyles(), since this function already has to detect each
// IMAGE_SENTINEL to give it its own page in the first place.
std::vector<size_t> buildPageIndex(const CjkChapterFileReader& text, const PageMetrics& metrics, bool kinsokuEnabled,
                                   std::vector<int>* outPageImageIndex = nullptr);

// Byte offset in `text` where the column starting at `offset` ends (i.e.
// the start of the next column), applying kinsoku if enabled. Shared by
// buildPageIndex() and the renderer's per-column draw loop so pagination
// and rendering always agree on exactly where each column breaks.
size_t nextColumnEnd(const CjkChapterFileReader& text, size_t offset, const PageMetrics& metrics, bool kinsokuEnabled);

// Copies text[start, end) into a small contiguous std::string. Used by the
// renderer to get a page's or column's worth of text (always small --
// bounded by rowsPerColumn * columnsPerPage codepoints) for the c_str()-based
// glyph/font APIs.
std::string extractRange(const CjkChapterFileReader& text, size_t start, size_t end);

// Style state (see isStyleSentinel/styleFromSentinel) active at the start of
// each page in `pageIndex`, computed by scanning for sentinel bytes between
// consecutive page boundaries -- a separate, simple linear byte scan rather
// than threading style through nextColumnEnd's kinsoku-adjusted scan window,
// since a chosen break can land before a sentinel that was inside the same
// scan window. Returned vector has one entry per page (pageIndex.size() - 1,
// same indexing as pageIndex itself minus its trailing sentinel entry).
std::vector<uint8_t> computePageStartStyles(const CjkChapterFileReader& text, const std::vector<size_t>& pageIndex);

}  // namespace CjkVerticalLayout
