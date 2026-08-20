#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits; render-time overlay bits do not affect font selection.
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  const EpdFont* font = getFont(style);
  const EpdGlyph* glyph = font->getGlyph(cp);
  // getFont() only falls back to regular when the requested style's font
  // is entirely absent (e.g. no bold file loaded) -- it has no way to know
  // if a style that IS loaded simply has incomplete coverage for this one
  // codepoint (a real, common case for SD card CJK fonts: a bold face often
  // covers far fewer ideographs than its regular counterpart). Without this,
  // a bold-styled CJK run silently drops exactly the characters bold is
  // missing while regular has them, which reads as "the title vanished" --
  // seen on a real device with a title reduced to just its embedded ASCII
  // digits once every CJK glyph in it hit this gap under BOLD.
  if (!glyph && font != regular) {
    glyph = regular->getGlyph(cp);
  }
  return glyph;
}

bool EpdFontFamily::hasCodepoint(const uint32_t cp, const Style style) const {
  const EpdFont* font = getFont(style);
  if (font->hasCodepoint(cp)) return true;
  // See getGlyph()'s comment: keep this in sync with it so a caller that
  // gates drawing on hasCodepoint() (e.g. GfxRenderer::resolveTextFontId's
  // CJK-fallback redirect) agrees with what getGlyph() will actually manage
  // to draw for the same codepoint/style.
  return font != regular && regular->hasCodepoint(cp);
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
