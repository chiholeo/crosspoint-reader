#include "CjkChapterText.h"

#include <Epub/htmlEntities.h>
#include <Esp.h>
#include <FsHelpers.h>

#include <algorithm>
#include <cctype>
#include <cstring>

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

// All six heading levels collapse to the same single "heading" text level
// (see the class comment): one size tier up from body text, not six
// different sizes -- CJK_READER_SETTINGS only has one reading font size
// loaded, plus whatever single larger size the renderer picks to stand in
// for "heading".
bool isHeadingTag(const std::string& name) {
  return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

// <b> and <strong> both mean "bold" for this reader's purposes -- no
// distinction between presentational and semantic emphasis, matching how
// isHeadingTag() above ignores heading-level nuance too.
bool isBoldTag(const std::string& name) { return name == "b" || name == "strong"; }

bool isImageTag(const std::string& name) { return name == "img"; }

// Finds attrName="..." or attrName='...' within html[start, end) (the
// attribute portion of one tag, after the tag name) and returns the
// unquoted value, or an empty string if not present. No other tag
// attribute needs parsing in this extractor (see the class comment for why
// this stays a minimal tag-stripper, not a general HTML/attribute parser),
// so this is scoped to exactly the one case <img src="..."> needs rather
// than a reusable general-purpose attribute scanner.
std::string extractAttributeValue(const char* html, const size_t start, const size_t end, const char* attrName) {
  const size_t attrLen = std::strlen(attrName);
  for (size_t i = start; i + attrLen < end; i++) {
    if (html[i] != attrName[0]) continue;
    if (strncmp(html + i, attrName, attrLen) != 0) continue;
    size_t j = i + attrLen;
    while (j < end && isAsciiSpace(html[j])) j++;
    if (j >= end || html[j] != '=') continue;
    j++;
    while (j < end && isAsciiSpace(html[j])) j++;
    if (j >= end || (html[j] != '"' && html[j] != '\'')) continue;
    const char quote = html[j];
    j++;
    const size_t valueStart = j;
    while (j < end && html[j] != quote) j++;
    if (j >= end) return "";  // unterminated -- malformed tag, no usable value
    return std::string(html + valueStart, j - valueStart);
  }
  return "";
}

// Upper bound on how long an unresolved '<...' or '&...' span is allowed to
// grow across feed() calls before giving up on interpreting it as a tag or
// entity. Real XHTML tags/entities are always far shorter than this --
// this exists purely so malformed/truncated content (a stray unmatched '<'
// with no '>' anywhere in the rest of the chapter) can't make `pending`
// grow unboundedly.
constexpr size_t kMaxPendingSpan = 4096;

// PARAGRAPH_SEPARATOR (U+2029)'s UTF-8 encoding -- a fixed, known codepoint,
// so this is hardcoded rather than needing a generic codepoint encoder.
constexpr char kParagraphSeparatorUtf8[3] = {static_cast<char>(0xE2), static_cast<char>(0x80),
                                             static_cast<char>(0xA9)};
// CjkVerticalLayout::STYLE_SENTINEL_NORMAL/HEADING/BOLD/HEADING_BOLD
// (U+E000-U+E003)'s UTF-8 encodings -- fixed, known codepoints, hardcoded
// the same way. Indexed directly by the 2-bit style value (see
// CjkVerticalLayout.h's STYLE_BIT_HEADING/STYLE_BIT_BOLD).
constexpr char kStyleSentinelUtf8[4][3] = {
    {static_cast<char>(0xEE), static_cast<char>(0x80), static_cast<char>(0x80)},  // normal
    {static_cast<char>(0xEE), static_cast<char>(0x80), static_cast<char>(0x81)},  // heading
    {static_cast<char>(0xEE), static_cast<char>(0x80), static_cast<char>(0x82)},  // bold
    {static_cast<char>(0xEE), static_cast<char>(0x80), static_cast<char>(0x83)},  // heading+bold
};
// CjkVerticalLayout::IMAGE_SENTINEL (U+E010)'s UTF-8 encoding -- a distinct
// PUA point from the style-sentinel range above, hardcoded the same way.
constexpr char kImageSentinelUtf8[3] = {static_cast<char>(0xEE), static_cast<char>(0x80), static_cast<char>(0x90)};
}  // namespace

namespace CjkChapterText {

bool ChapterTextExtractor::beginWrite(const std::string& path, const std::string& baseDir) {
  contentBaseDir = baseDir;
  imagePaths.clear();
  file = Storage.open(path.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    ioError_ = true;
    return false;
  }
  return true;
}

bool ChapterTextExtractor::flushBuf() {
  if (writeBufLen == 0) return true;
  const size_t written = file.write(writeBuf, writeBufLen);
  const bool ok = written == writeBufLen;
  if (!ok) ioError_ = true;
  writeBufLen = 0;
  return ok;
}

void ChapterTextExtractor::appendChar(const char c) {
  if (ioError_) return;
  if (!pendingTrim.empty()) {
    for (const char t : pendingTrim) {
      if (writeBufLen >= kWriteBufSize && !flushBuf()) return;
      writeBuf[writeBufLen++] = t;
      totalBytes++;
    }
    pendingTrim.clear();
  }
  if (writeBufLen >= kWriteBufSize && !flushBuf()) return;
  writeBuf[writeBufLen++] = c;
  totalBytes++;
}

void ChapterTextExtractor::appendStr(const char* s) {
  for (; *s; s++) appendChar(*s);
}

void ChapterTextExtractor::feed(const char* data, const size_t len) {
  if (ioError_) {
    return;
  }
  if (pending.empty()) {
    processChunk(data, len, false);
    return;
  }
  // Prepend the carried-over partial tag/entity from the previous chunk.
  // pending is bounded (kMaxPendingSpan) and len is bounded (the caller's
  // own stream chunk size), so this combined buffer is small and
  // fixed-size (a few KB) -- but it's still a real allocation this build
  // can't recover from if it fails (no exception handling: an earlier
  // version of this extractor hit exactly that, via addr2line:
  // basic_string::_M_append -> _M_mutate -> operator new -> bad_alloc ->
  // terminate). A lightweight sanity check is enough here, though: unlike
  // that earlier version, this extractor no longer holds the chapter's
  // growing text in RAM at all (see the class-level comment), so there's no
  // structural reason this small, fixed-size temporary should ever be
  // tight on headroom -- this is a last-resort guard, not the routine case.
  if (ESP.getMaxAllocHeap() < pending.size() + len + 2048) {
    ioError_ = true;
    return;
  }
  std::string buf = std::move(pending);
  pending.clear();
  buf.append(data, len);
  processChunk(buf.data(), buf.size(), false);
}

size_t ChapterTextExtractor::finish() {
  if (!ioError_ && !pending.empty()) {
    std::string leftover = std::move(pending);
    pending.clear();
    processChunk(leftover.data(), leftover.size(), true);
  }
  // pendingTrim (a trailing space or paragraph separator that hasn't been
  // committed yet -- see appendChar) is simply left unflushed here, never
  // written: trimmable spans are deferred until real text follows them
  // (rather than written optimistically and trimmed back afterward, which
  // would need truncating an already-written file), so reaching finish()
  // while one is still pending means it was genuinely trailing and is
  // correctly dropped by doing nothing.
  flushBuf();
  file.close();
  return ioError_ ? 0 : totalBytes;
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

        if (isBlockTag(tagName) && !lastWasSpace && totalBytes > 0) {
          pendingTrim.assign(kParagraphSeparatorUtf8, 3);
          lastWasSpace = true;  // suppress a redundant separator/space right after
        }

        bool styleTagSeen = false;
        if (isHeadingTag(tagName)) {
          if (closing) {
            if (headingDepth > 0) headingDepth--;
          } else {
            headingDepth++;
          }
          styleTagSeen = true;
        }
        if (isBoldTag(tagName)) {
          if (closing) {
            if (boldDepth > 0) boldDepth--;
          } else {
            boldDepth++;
          }
          styleTagSeen = true;
        }
        if (styleTagSeen) {
          const uint8_t newStyle = (headingDepth > 0 ? CjkVerticalLayout::STYLE_BIT_HEADING : 0) |
                                   (boldDepth > 0 ? CjkVerticalLayout::STYLE_BIT_BOLD : 0);
          if (newStyle != currentStyle) {
            currentStyle = newStyle;
            const char* sentinel = kStyleSentinelUtf8[newStyle];
            appendChar(sentinel[0]);
            appendChar(sentinel[1]);
            appendChar(sentinel[2]);
          }
        }

        // <img> is void/self-closing (no separate </img> in real markup),
        // so only the opening form needs handling. src is resolved the
        // same way Epub's own OPF/nav parsers resolve a relative href:
        // relative to the current document's own directory, URI-unescaped,
        // then path-normalised (collapsing "../" etc.).
        if (!closing && isImageTag(tagName)) {
          const std::string src = extractAttributeValue(html, nameEnd, gt, "src");
          if (!src.empty()) {
            imagePaths.push_back(FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(contentBaseDir + src)));
            appendChar(kImageSentinelUtf8[0]);
            appendChar(kImageSentinelUtf8[1]);
            appendChar(kImageSentinelUtf8[2]);
          }
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
          appendStr(decoded);
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
      appendChar(c);
      lastWasSpace = false;
      i++;
      continue;
    }

    if (isAsciiSpace(c)) {
      if (!lastWasSpace && totalBytes > 0) {
        pendingTrim = " ";
        lastWasSpace = true;
      }
      i++;
      continue;
    }

    appendChar(c);
    lastWasSpace = false;
    i++;
  }
}

}  // namespace CjkChapterText
