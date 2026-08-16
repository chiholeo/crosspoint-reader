#pragma once

#include <cstdint>

// Kinsoku shori (Japanese/Chinese line-wrap rules) for the CJK vertical
// reader. Pure codepoint-table logic: no SD, framebuffer, or font
// dependency, so it works unchanged regardless of rendering substrate.
//
// Rule tables and the offset trick in getVerticalPunctuationOffset() are
// ported from aBER0724/crosspoint-reader-cjk's src/reader/LayoutEngine.cpp
// (a real, shipped CJK fork) — see add_CJKreader.md for why only this piece
// was adopted from that codebase.
namespace CjkKinsoku {

// True for closing punctuation that must never start a column
// (e.g. "。" at the top of a new column reads wrong — it belongs at the
// bottom of the previous one).
bool isProhibitedLineStart(uint32_t cp);

// True for opening punctuation that must never end a column.
bool isProhibitedLineEnd(uint32_t cp);

// True for small marks (commas, periods, colons, ! ?) that read correctly
// with a positional nudge in tategaki (vertical) layout -- their glyph
// shape isn't inherently horizontal the way a bracket or quote is.
bool isVerticalPunctuation(uint32_t cp);

// Pixel nudge to apply when drawing a vertical-punctuation glyph upright.
// (0, 0) for codepoints isVerticalPunctuation() reports false for.
void getVerticalPunctuationOffset(uint32_t cp, uint16_t glyphW, uint16_t glyphH, int8_t& outDx, int8_t& outDy);

// True for bracket- and quote-shaped marks (CJK corner brackets, fullwidth
// parentheses, angle brackets, curly quotes) whose glyph is drawn for
// horizontal text and reads sideways in a vertical column no matter how
// it's nudged. These need an actual 90-degree rotated draw
// (GfxRenderer::drawTextRotated90CW), not an offset.
bool isRotatedPunctuation(uint32_t cp);

// Given a column's codepoints (in reading order) and the maximum codepoints
// that fit in one column, returns how many codepoints actually belong in
// this column once kinsoku rules are applied — walking the break point back
// (never ending on a prohibited-end char) or forward (never starting a
// prohibited-start char) from the naive break at maxColumns.
// Returns 0 if count or maxColumns is 0.
uint8_t chooseLineBreak(const uint32_t* lineCodepoints, uint8_t count, uint16_t maxColumns);

}  // namespace CjkKinsoku
