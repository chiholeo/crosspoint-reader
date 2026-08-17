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

// Upper bound on how long an unresolved '<...' or '&...' span is allowed to
// grow across feed() calls before giving up on interpreting it as a tag or
// entity. Real XHTML tags/entities are always far shorter than this --
// this exists purely so malformed/truncated content (a stray unmatched '<'
// with no '>' anywhere in the rest of the chapter) can't make `pending`
// grow to hold the whole remaining chapter, which would recreate the exact
// single-large-buffer problem streaming was added to avoid.
constexpr size_t kMaxPendingSpan = 4096;
}  // namespace

namespace CjkChapterText {

void ChapterTextExtractor::feed(const char* data, const size_t len) {
  if (pending.empty()) {
    processChunk(data, len, false);
    return;
  }
  // Prepend the carried-over partial tag/entity from the previous chunk.
  // pending is bounded (kMaxPendingSpan) so this copy is small.
  std::string buf = std::move(pending);
  pending.clear();
  buf.append(data, len);
  processChunk(buf.data(), buf.size(), false);
}

std::string ChapterTextExtractor::finish() {
  if (!pending.empty()) {
    std::string leftover = std::move(pending);
    pending.clear();
    processChunk(leftover.data(), leftover.size(), true);
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
  return std::move(out);
}

void ChapterTextExtractor::processChunk(const char* html, const size_t htmlLen, const bool flush) {
  size_t i = 0;

  while (i < htmlLen) {
    const char c = html[i];

    if (c == '<') {
      size_t gt = i;
      while (gt < htmlLen && html[gt] != '>') gt++;

      if (gt >= htmlLen) {
        // Tag doesn't close within what we have.
        if (!flush && (htmlLen - i) < kMaxPendingSpan) {
          pending.assign(html + i, htmlLen - i);
          return;
        }
        // Flushing, or the span grew implausibly large (malformed content):
        // fall through and treat the rest of this chunk as literal text.
      } else {
        const size_t tagStart = i + 1;
        const bool closing = tagStart < htmlLen && html[tagStart] == '/';
        const size_t nameStart = closing ? tagStart + 1 : tagStart;
        size_t nameEnd = nameStart;
        while (nameEnd < gt && (std::isalnum(static_cast<unsigned char>(html[nameEnd])) != 0 || html[nameEnd] == '-')) {
          nameEnd++;
        }
        std::string tagName;
        tagName.reserve(nameEnd - nameStart);
        for (size_t k = nameStart; k < nameEnd; k++) {
          tagName += static_cast<char>(std::tolower(static_cast<unsigned char>(html[k])));
        }

        const size_t nextI = gt + 1;

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
      const size_t window = i + 12;
      const size_t limit = std::min(htmlLen, window);
      size_t semi = i + 1;
      while (semi < limit && html[semi] != ';') semi++;

      if (semi < limit && html[semi] == ';') {
        if (const char* decoded = lookupHtmlEntity(html + i, semi - i + 1)) {
          out += decoded;
          lastWasSpace = false;
          i = semi + 1;
          continue;
        }
        // Recognized-length span but not a known entity -- fall through to
        // literal '&' handling below.
      } else if (limit == htmlLen && htmlLen < window && !flush) {
        // Hit the end of what we have before completing the lookahead
        // window -- the ';' could still be just past this chunk. Defer
        // (bounded: at most 12 bytes, well under kMaxPendingSpan).
        pending.assign(html + i, htmlLen - i);
        return;
      }
      // else: window fully scanned (or flushing) with no ';' -- literal '&'.
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
}

std::string extractPlainText(const char* html, const size_t htmlLen) {
  ChapterTextExtractor extractor;
  extractor.feed(html, htmlLen);
  return extractor.finish();
}

}  // namespace CjkChapterText
