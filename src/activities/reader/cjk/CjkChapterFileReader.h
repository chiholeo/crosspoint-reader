#pragma once
#include <HalStorage.h>

#include <cstddef>
#include <string>

// Read-only, random-access view over a chapter's extracted plain text,
// stored on SD rather than resident in RAM -- see CjkChapterText.h for why.
// On-device testing found extraction still hitting total heap exhaustion
// around ~83KB even after the in-RAM text storage's allocation strategy
// (CjkChunkedText, now removed) was tuned to its practical floor: the
// decompressor's own fixed ~44KB (state+window, see
// lib/miniz/src/InflateStream.h) plus a large chapter's own text left
// nothing in reserve, no matter how efficiently the text's bytes were
// packed. Reading small, bounded ranges from disk on demand instead of
// holding any meaningful fraction of the chapter resident makes RAM usage
// O(1) in chapter size -- a handful of small buffers -- rather than O(chapter
// size), removing the ceiling instead of squeezing under it.
class CjkChapterFileReader {
 public:
  CjkChapterFileReader() = default;

  // Opens `path` for reading and caches its size. Returns false on failure
  // (leaves the reader in a valid, empty state -- size() == 0).
  bool open(const std::string& path);

  size_t size() const { return fileSize; }
  bool isOpen() const { return file.isOpen(); }

  // Reads text[start, start+len) into dest. Returns the number of bytes
  // actually read (less than len only when start+len exceeds size(), i.e.
  // reading off the end). Bytes beyond what's read are left untouched --
  // callers that need a deterministic tail (e.g. zero-padding for a
  // truncated codepoint scan near the chapter's end) should zero `dest`
  // themselves first, matching CjkVerticalLayout's existing discipline.
  size_t readRange(size_t start, char* dest, size_t len) const;

 private:
  // seek()/read() are physically non-const on HalFile, but readRange() is
  // logically a pure lookup from a caller's perspective (matches the
  // const-ref style CjkVerticalLayout's functions already use for their
  // text-source parameter) -- mutable keeps that interface.
  mutable HalFile file;
  size_t fileSize = 0;
};
