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

// Incremental extractor: feed() raw HTML bytes as they arrive from a
// streamed SD/zip read (Epub::readItemContentsToStream), rather than
// requiring the whole chapter's raw HTML in one contiguous buffer first.
// On-device testing found chapters whose raw HTML exceeds the largest free
// heap block even when total free heap is healthy -- reading the whole
// chapter into one buffer before stripping it down means paying that
// buffer's peak size for the LARGER raw-markup form, when the actual output
// (plain text, tags/attributes stripped) is usually meaningfully smaller.
// Streaming avoids ever holding more than one chunk of raw HTML plus the
// growing output string.
class ChapterTextExtractor {
 public:
  // Feed the next chunk of raw HTML bytes. Chunks do not need to align with
  // tag or entity boundaries -- an incomplete "<...", "&...;" span at the
  // end of a chunk is carried over (bounded) and resolved once the rest
  // arrives in a later feed() call.
  void feed(const char* data, size_t len);

  // Optional: reserve capacity for the output text up front (e.g. from the
  // source item's known raw byte size) so feed() never needs a mid-stream
  // reallocation. Plain text is always <= raw HTML size, so reserving the
  // raw size guarantees this. Safe to skip; feed() grows `out` on demand
  // either way.
  void reserveCapacity(size_t n) { out.reserve(n); }

  // Call once after the last feed(). Flushes any still-pending partial
  // token (tolerated as literal text -- true truncation mid-tag/entity is
  // rare and not worth failing the whole chapter over) and returns the
  // accumulated plain text.
  std::string finish();

 private:
  std::string out;
  bool lastWasSpace = true;   // suppresses a leading/duplicate space or paragraph separator
  std::string skipUntilTag;   // non-empty while inside <script>/<style>/<head>, persists across feed() calls
  std::string pending;        // carries an incomplete '<'/'&' span into the next feed() call

  // Processes html[0, htmlLen); on hitting an unresolved '<' or '&' span
  // that doesn't close within this call's data, stashes the remainder into
  // `pending` and returns (unless flush is true, in which case it's
  // emitted as literal text instead).
  void processChunk(const char* html, size_t htmlLen, bool flush);
};

// One-shot convenience wrapper for callers that already have the whole
// chapter in memory. Equivalent to a single feed() + finish().
std::string extractPlainText(const char* html, size_t htmlLen);

}  // namespace CjkChapterText
