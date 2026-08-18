#include "CjkChapterFileReader.h"

#include <algorithm>

bool CjkChapterFileReader::open(const std::string& path) {
  file = Storage.open(path.c_str(), O_RDONLY);
  if (!file) {
    fileSize = 0;
    return false;
  }
  fileSize = file.fileSize();
  return true;
}

size_t CjkChapterFileReader::readRange(const size_t start, char* dest, const size_t len) const {
  if (!file.isOpen() || start >= fileSize || len == 0) return 0;
  const size_t clampedLen = std::min(len, fileSize - start);
  if (!file.seek(start)) return 0;
  const int n = file.read(dest, clampedLen);
  return n > 0 ? static_cast<size_t>(n) : 0;
}
