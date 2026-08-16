#include "CjkChapterText.h"

#include <Epub/htmlEntities.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>

#include "CjkVerticalLayout.h"

namespace {
bool isAsciiSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Tags whose entire content (including nested markup) must be dropped.
bool isSkipTag(const std::string& name) { return name == "script" || name == "style" || name == "head"; }

// Tags that mark a paragraph/block boundary. Emitted as PARAGRAPH_SEPARATOR
// (U+2029) rather than a plain space -- CjkVerticalLayout treats that
// codepoint as a forced column break, so paragraphs each start at the top
// of a fresh column instead of running together mid-column.
bool isBlockTag(const std::string& name) {
  return name == "p" || name == "div" || name == "li" || name == "br" || name == "h1" || name == "h2" ||
         name == "h3" || name == "h4" || name == "h5" || name == "h6" || name == "blockquote";
}
}  // namespace

namespace CjkChapterText {

std::string extractPlainText(const char* html, const size_t htmlLen) {
  std::string out;
  out.reserve(htmlLen / 2);

  size_t i = 0;
  bool lastWasSpace = true;  // suppresses a leading space
  std::string skipUntilTag;  // non-empty while inside <script>/<style>/<head>

  while (i < htmlLen) {
    const char c = html[i];

    if (c == '<') {
      const size_t tagStart = i + 1;
      const bool closing = tagStart < htmlLen && html[tagStart] == '/';
      const size_t nameStart = closing ? tagStart + 1 : tagStart;
      size_t nameEnd = nameStart;
      while (nameEnd < htmlLen &&
             (std::isalnum(static_cast<unsigned char>(html[nameEnd])) != 0 || html[nameEnd] == '-')) {
        nameEnd++;
      }
      std::string tagName;
      tagName.reserve(nameEnd - nameStart);
      for (size_t k = nameStart; k < nameEnd; k++) {
        tagName += static_cast<char>(std::tolower(static_cast<unsigned char>(html[k])));
      }

      size_t gt = i;
      while (gt < htmlLen && html[gt] != '>') gt++;
      const size_t nextI = (gt < htmlLen) ? gt + 1 : htmlLen;

      if (!skipUntilTag.empty()) {
        if (closing && tagName == skipUntilTag) skipUntilTag.clear();
        i = nextI;
        continue;
      }

      if (!closing && isSkipTag(tagName)) {
        skipUntilTag = tagName;
        i = nextI;
        continue;
      }

      if (isBlockTag(tagName) && !lastWasSpace && !out.empty()) {
        utf8AppendCodepoint(CjkVerticalLayout::PARAGRAPH_SEPARATOR, out);
        lastWasSpace = true;  // suppress a redundant separator/space right after
      }

      i = nextI;
      continue;
    }

    if (!skipUntilTag.empty()) {
      i++;
      continue;
    }

    // Drop NUL and other non-printable C0 control bytes (whitespace already
    // handled below). Most commonly seen when a chapter's actual bytes don't
    // match its declared encoding (e.g. a stray UTF-16-encoded chapter in an
    // otherwise-UTF-8 EPUB, which is mostly NUL bytes interleaved with
    // ASCII). NUL must never reach the output: utf8NextCodepoint() treats it
    // as a C-string terminator and won't advance past it, so an embedded NUL
    // would hang CjkVerticalLayout's column-break scan.
    if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') {
      i++;
      continue;
    }

    if (c == '&') {
      const size_t limit = std::min(htmlLen, i + 12);
      size_t semi = i + 1;
      while (semi < limit && html[semi] != ';') semi++;
      if (semi < limit && html[semi] == ';') {
        if (const char* decoded = lookupHtmlEntity(html + i, semi - i + 1)) {
          out += decoded;
          lastWasSpace = false;
          i = semi + 1;
          continue;
        }
      }
      out += c;
      lastWasSpace = false;
      i++;
      continue;
    }

    if (isAsciiSpace(c)) {
      if (!lastWasSpace && !out.empty()) {
        out += ' ';
        lastWasSpace = true;
      }
      i++;
      continue;
    }

    out += c;
    lastWasSpace = false;
    i++;
  }

  while (!out.empty()) {
    if (out.back() == ' ') {
      out.pop_back();
    } else if (out.size() >= 3 && static_cast<unsigned char>(out[out.size() - 3]) == 0xE2 &&
               static_cast<unsigned char>(out[out.size() - 2]) == 0x80 &&
               static_cast<unsigned char>(out[out.size() - 1]) == 0xA9) {
      out.resize(out.size() - 3);  // trailing PARAGRAPH_SEPARATOR (U+2029, 3 UTF-8 bytes)
    } else {
      break;
    }
  }
  return out;
}

}  // namespace CjkChapterText
