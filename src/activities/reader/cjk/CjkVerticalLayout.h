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
std::vector<size_t> buildPageIndex(const CjkChapterFileReader& text, const PageMetrics& metrics, bool kinsokuEnabled);

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

}  // namespace CjkVerticalLayout
