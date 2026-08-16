#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Vertical-column pagination for the CJK reader. Pure pagination math over
// an already-extracted plain-text chapter (see CjkChapterText) -- no SD,
// framebuffer, or font-object access of its own. Font metrics come in as
// plain ints the caller already measured, so this stays substrate-agnostic.
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
// byte range. Built once per chapter and kept in RAM -- a chapter's plain
// text is small enough (tens of KB at most) that this is far cheaper than
// the standard reader's disk-backed section cache, and this engine doesn't
// need that cache's incremental-build/resume machinery for a v1.
std::vector<size_t> buildPageIndex(const std::string& text, const PageMetrics& metrics, bool kinsokuEnabled);

// Byte offset in `text` where the column starting at `offset` ends (i.e.
// the start of the next column), applying kinsoku if enabled. Shared by
// buildPageIndex() and the renderer's per-column draw loop so pagination
// and rendering always agree on exactly where each column breaks.
size_t nextColumnEnd(const std::string& text, size_t offset, const PageMetrics& metrics, bool kinsokuEnabled);

}  // namespace CjkVerticalLayout
