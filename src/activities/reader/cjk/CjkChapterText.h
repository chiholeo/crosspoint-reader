#pragma once
#include <string>

// Minimal HTML-to-plain-text extraction for the CJK vertical reader.
//
// Deliberately NOT a reuse of lib/Epub's ChapterHtmlSlimParser: that parser
// is tightly coupled to Page/TextBlock/image handling for the standard
// reader's layout pipeline, so pulling it in would drag in far more than a
// plain-text stream needs. This is a small, self-contained tag-stripper
// scoped to what a v1 CJK reader needs: no CSS awareness, no images, no
// paragraph-level styling -- just readable text, in reading order.
namespace CjkChapterText {

// Strips tags, decodes HTML entities (via lib/Epub/Epub/htmlEntities.h),
// and collapses runs of whitespace. Block-level tag boundaries (p, div, li,
// br, headings) become a single space in the output stream -- there is no
// paragraph-level layout in the v1 CJK vertical layout, just a continuous
// column-broken character stream.
std::string extractPlainText(const char* html, size_t htmlLen);

}  // namespace CjkChapterText
