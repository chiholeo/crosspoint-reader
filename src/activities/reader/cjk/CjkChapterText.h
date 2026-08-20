#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Minimal HTML-to-plain-text extraction for the CJK vertical reader.
//
// Deliberately NOT a reuse of lib/Epub's ChapterHtmlSlimParser: that parser
// is tightly coupled to Page/TextBlock handling for the standard reader's
// full layout pipeline (inline flow around images, etc.), so pulling it in
// would drag in far more than this reader needs. This is a small,
// self-contained tag-stripper scoped to what a v1 CJK reader needs: no
// general CSS awareness, no inline image flow -- just readable text, in
// reading order, with <h1>-<h6> headings and <b>/<strong> emphasis
// preserved as sentinel markers (see CjkVerticalLayout.h's
// STYLE_SENTINEL_BASE) rather than dropped like all other markup, so the
// renderer can show them larger and/or bold. <img> tags are preserved too,
// as a distinct sentinel (see CjkVerticalLayout.h's IMAGE_SENTINEL) paired
// with a resolved href in getImagePaths() -- the renderer gives each one a
// full dedicated page rather than attempting inline flow.
namespace CjkChapterText {

// Incremental extractor: feed() raw HTML bytes as they arrive from a
// streamed SD/zip read (Epub::readItemContentsToStream), rather than
// requiring the whole chapter's raw HTML in one contiguous buffer first.
//
// Output is written to an SD file through a small fixed-size buffer, not
// accumulated in RAM at all (compare: an earlier version tried a std::string,
// then a custom RAM-resident chunked buffer with a dynamically-sized,
// adaptively-shrinking chunk -- both eventually traced back to genuine total
// heap exhaustion on-device, not an allocation-strategy bug: the zip/inflate
// stream's own fixed ~44KB decompressor state+window (see
// lib/miniz/src/InflateStream.h) plus a large chapter's extracted text left
// nothing in reserve, however efficiently that text's bytes were packed).
// Writing straight to disk through a small bounded buffer makes this
// extractor's own RAM usage O(1) in chapter size, independent of how large
// the chapter's plain text turns out to be. See CjkChapterFileReader for the
// read-back side used by pagination and rendering.
class ChapterTextExtractor {
 public:
  // Opens (creates/truncates) `path` as the output file. Must succeed
  // before feed() is called -- feed() is a silent no-op (ioError() latches
  // true) if it wasn't. `path`'s parent directory must already exist.
  // `contentBaseDir` is the current chapter's own directory within the
  // EPUB (e.g. "OEBPS/Text/" for a chapter at that path) -- needed to
  // resolve a relative <img src="..."> into a real, zip-lookupable href,
  // the same way Epub's own OPF/nav parsers resolve hrefs relative to
  // their source document.
  bool beginWrite(const std::string& path, const std::string& contentBaseDir = "");

  // Resolved hrefs for each <img> encountered, in extraction order. Each
  // IMAGE_SENTINEL in the extracted text stream corresponds, positionally,
  // to one entry here (the Nth sentinel -> imagePaths[N]) -- the sentinel
  // itself is zero-width and can't carry a variable-length path inline.
  const std::vector<std::string>& getImagePaths() const { return imagePaths; }

  // Feed the next chunk of raw HTML bytes. Chunks do not need to align with
  // tag or entity boundaries -- an incomplete "<...", "&...;" span at the
  // end of a chunk is carried over (bounded) and resolved once the rest
  // arrives in a later feed() call.
  void feed(const char* data, size_t len);

  // True if beginWrite() failed, or a write to the output file failed
  // partway through (SD full/removed/etc. -- distinct from the old
  // allocation-failure OOM path, which no longer applies here).
  bool ioError() const { return ioError_; }

  size_t bytesWrittenSoFar() const { return totalBytes; }

  // Call once after the last feed(). Flushes any still-pending partial
  // token (tolerated as literal text -- true truncation mid-tag/entity is
  // rare and not worth failing the whole chapter over) and the write
  // buffer, then closes the output file. Returns the total number of plain
  // text bytes written (0 if ioError()).
  size_t finish();

 private:
  HalFile file;
  static constexpr size_t kWriteBufSize = 2048;
  char writeBuf[kWriteBufSize];
  size_t writeBufLen = 0;
  size_t totalBytes = 0;
  bool ioError_ = false;

  bool lastWasSpace = true;   // suppresses a leading/duplicate space or paragraph separator
  std::string skipUntilTag;   // non-empty while inside <script>/<style>/<head>, persists across feed() calls
  std::string pending;        // carries an incomplete '<'/'&' span into the next feed() call
  // A trimmable space or paragraph separator seen but not yet written --
  // deferred until real text follows it (appendChar/appendStr flush this
  // first), so a run that turns out to be trailing (nothing more follows
  // before finish()) is simply never written, instead of being written
  // optimistically and needing the file truncated afterward.
  std::string pendingTrim;
  // Depth counters, not bools: handle nested/malformed <h*>/<b>/<strong>
  // markup (e.g. a stray unmatched close) without going negative or getting
  // stuck "on". The two track independently -- bold heading text is a real,
  // valid combination -- and currentStyle (see CjkVerticalLayout.h's 2-bit
  // sentinel scheme) is only re-emitted when the combined value actually
  // changes, not on every tag.
  int headingDepth = 0;
  int boldDepth = 0;
  uint8_t currentStyle = 0;
  std::string contentBaseDir;         // set once in beginWrite(), used to resolve <img src="...">
  std::vector<std::string> imagePaths;  // one entry per <img> found, in extraction order

  // Flushes writeBuf to `file`. Sets ioError_ and returns false on a short
  // write; writeBuf is reset to empty either way.
  bool flushBuf();
  void appendChar(char c);
  void appendStr(const char* s);

  // Processes html[0, htmlLen); on hitting an unresolved '<' or '&' span
  // that doesn't close within this call's data, stashes the remainder into
  // `pending` and returns (unless flush is true, in which case it's
  // emitted as literal text instead).
  void processChunk(const char* html, size_t htmlLen, bool flush);
};

}  // namespace CjkChapterText
