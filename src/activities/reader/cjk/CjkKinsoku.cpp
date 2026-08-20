#include "CjkKinsoku.h"

#include <algorithm>
#include <cstddef>

namespace {
// Closing punctuation / terminators: forbidden at the start of a column.
constexpr uint32_t kLineStartForbidden[] = {
    0x3002, 0xFF0C, 0x3001, 0x300D, 0x300F, 0xFF09, 0x300B, 0xFF1B, 0xFF1A, 0xFF01,
    0xFF1F, 0x0025, 0x002E, 0x005D,
};

// Opening punctuation: forbidden at the end of a column.
constexpr uint32_t kLineEndForbidden[] = {
    0x300C, 0x300E, 0xFF08, 0x300A, 0x005B,
};

// Small marks that read correctly with a positional nudge rather than a
// full glyph rotation in vertical layout.
constexpr uint32_t kVerticalPunctuation[] = {
    0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF01, 0xFF1F,
};

// Bracket- and quote-shaped marks: their glyphs are drawn for horizontal
// text, so a corner bracket or curly quote reads sideways in a vertical
// column no matter how it's nudged -- these need an actual rotated draw.
// Both members of each opening/closing pair are listed (e.g. both 「 and 」,
// not just the closing one) -- treating only one half left the other
// rendering unrotated, which is what looked like "no fix" after the first
// attempt covered curly quotes only.
constexpr uint32_t kRotatedPunctuation[] = {
    0x300C, 0x300D,  // 「 」 corner brackets (the common CJK quotation mark)
    0x300E, 0x300F,  // 『 』 white corner brackets (nested/title quotes)
    0xFF08, 0xFF09,  // （ ） fullwidth parentheses
    0x0028, 0x0029,  // ( )  ASCII parentheses -- mixed CJK/Latin text uses these too
    0x005B, 0x005D,  // [ ]  ASCII square brackets (already in the kinsoku tables below)
    0x300A, 0x300B,  // 《 》 double angle brackets (book/work titles)
    0x3008, 0x3009,  // 〈 〉 single angle brackets
    0x2018, 0x2019,  // ‘ ’ curly single quotes
    0x201C, 0x201D,  // “ ” curly double quotes
    0x2014, 0x2015,  // — ― em dash / horizontal bar -- reads as a horizontal
                     // line in a vertical column no matter how it's nudged;
                     // convention rotates it to read as a vertical line.
                     // Chinese text commonly doubles it ("——"), which just
                     // means two consecutive rotated glyphs here.
    0x2026,          // … horizontal ellipsis -- same reasoning: reads
                     // sideways unrotated, rotates to a vertical row of dots.
};

// Subset of kRotatedPunctuation whose ink sits centered within its own
// bitmap box by design -- see isCenteredRotation()'s declaration.
constexpr uint32_t kCenteredRotation[] = {
    0x2014, 0x2015, 0x2026,
};

// Horizontal codepoint -> Unicode Vertical Forms (U+FE30-FE4F) equivalent.
// Both ASCII and fullwidth parens map to the same fullwidth vertical-paren
// glyph -- there's no separate "narrow" vertical form, and the rotation
// path already treats them identically. Deliberately omits: square brackets
// (no vertical form exists), curly quotes (none exists), 0x2015/ellipsis
// (already validated correct via centered rotation, and no vertical form
// exists for either) -- see verticalFormFor()'s own declaration.
struct VerticalFormEntry {
  uint32_t from;
  uint32_t to;
};
constexpr VerticalFormEntry kVerticalForms[] = {
    {0x300C, 0xFE41},  // 「 -> vertical left corner bracket
    {0x300D, 0xFE42},  // 」 -> vertical right corner bracket
    {0x300E, 0xFE43},  // 『 -> vertical left white corner bracket
    {0x300F, 0xFE44},  // 』 -> vertical right white corner bracket
    {0xFF08, 0xFE35},  // （ -> vertical left paren
    {0xFF09, 0xFE36},  // ） -> vertical right paren
    {0x0028, 0xFE35},  // (  -> vertical left paren (same glyph as fullwidth)
    {0x0029, 0xFE36},  // )  -> vertical right paren
    {0x300A, 0xFE3D},  // 《 -> vertical left double angle bracket
    {0x300B, 0xFE3E},  // 》 -> vertical right double angle bracket
    {0x3008, 0xFE3F},  // 〈 -> vertical left angle bracket
    {0x3009, 0xFE40},  // 〉 -> vertical right angle bracket
    {0x2014, 0xFE31},  // — -> vertical em dash
};

bool contains(const uint32_t* values, const size_t count, const uint32_t cp) {
  for (size_t i = 0; i < count; ++i) {
    if (values[i] == cp) return true;
  }
  return false;
}
}  // namespace

namespace CjkKinsoku {

bool isProhibitedLineStart(const uint32_t cp) {
  return contains(kLineStartForbidden, sizeof(kLineStartForbidden) / sizeof(kLineStartForbidden[0]), cp);
}

bool isProhibitedLineEnd(const uint32_t cp) {
  return contains(kLineEndForbidden, sizeof(kLineEndForbidden) / sizeof(kLineEndForbidden[0]), cp);
}

bool isVerticalPunctuation(const uint32_t cp) {
  return contains(kVerticalPunctuation, sizeof(kVerticalPunctuation) / sizeof(kVerticalPunctuation[0]), cp);
}

void getVerticalPunctuationOffset(const uint32_t cp, const uint16_t glyphW, const uint16_t glyphH, int8_t& outDx,
                                  int8_t& outDy) {
  outDx = 0;
  outDy = 0;
  if (!isVerticalPunctuation(cp)) return;

  // Tategaki punctuation sits slightly toward the top-right corner of its
  // glyph cell rather than centered — cheaper than a rotated draw and reads
  // correctly for the common marks (commas, periods, brackets, colons).
  outDx = static_cast<int8_t>(glyphW / 4);
  outDy = static_cast<int8_t>(-static_cast<int16_t>(glyphH / 6));
}

bool isRotatedPunctuation(const uint32_t cp) {
  return contains(kRotatedPunctuation, sizeof(kRotatedPunctuation) / sizeof(kRotatedPunctuation[0]), cp);
}

bool isCenteredRotation(const uint32_t cp) {
  return contains(kCenteredRotation, sizeof(kCenteredRotation) / sizeof(kCenteredRotation[0]), cp);
}

uint32_t verticalFormFor(const uint32_t cp) {
  for (const auto& entry : kVerticalForms) {
    if (entry.from == cp) return entry.to;
  }
  return 0;
}

uint8_t chooseLineBreak(const uint32_t* lineCodepoints, const uint8_t count, const uint16_t maxColumns) {
  if (count == 0 || maxColumns == 0) return 0;
  if (count <= maxColumns) return count;

  uint8_t breakIndex = static_cast<uint8_t>(maxColumns);
  if (breakIndex >= count) breakIndex = count - 1;

  // Never end a column on an opening bracket/quote.
  while (breakIndex > 1 && isProhibitedLineEnd(lineCodepoints[breakIndex - 1])) {
    --breakIndex;
  }

  // Never start the next column on a closing bracket/terminator. Capped at
  // maxColumns, not count-1: count-1 is the kinsoku lookahead buffer size
  // (rowsPerColumn + KINSOKU_LOOKAHEAD), and pushing breakIndex past
  // maxColumns would hand the caller a column with more codepoints than
  // the viewport has rows for -- the extra rows render past the bottom
  // margin, into/under the status bar. A short column (blank space at the
  // bottom) is the acceptable tradeoff; an overlong one is not.
  const uint8_t forwardLimit = static_cast<uint8_t>(std::min<uint16_t>(maxColumns, count - 1));
  while (breakIndex < forwardLimit && isProhibitedLineStart(lineCodepoints[breakIndex])) {
    ++breakIndex;
  }

  return breakIndex == 0 ? 1 : breakIndex;
}

}  // namespace CjkKinsoku
