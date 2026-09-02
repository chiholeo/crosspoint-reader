#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  void parseCssFiles() const;
  // Shared JPG/PNG -> BMP conversion used by both generateCoverBmp() (a
  // fixed, well-known item) and generateImageBmp() (an arbitrary in-book
  // image, cache-keyed by href hash since hrefs aren't valid filenames).
  bool convertItemToBmp(const std::string& itemHref, const std::string& outputBmpPath, bool cropped) const;

 public:
  // Per-book choice of which reader activity to open the EPUB in, cached on
  // disk so ReaderActivity only has to prompt the user once per book (see
  // getSavedReaderChoice()/saveReaderChoice()). Unset means "never asked, or
  // the cache was cleared" -- ReaderActivity prompts again in that case.
  enum class ReaderChoice : uint8_t { Unset = 0, Default = 1, Cjk = 2 };

  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  // Cache path/generator for an arbitrary in-book image (e.g. a CJK reader
  // full-page image break), keyed by a hash of itemHref since hrefs contain
  // '/' and aren't valid flat filenames. Same JPG/PNG support and
  // already-cached short-circuit as generateCoverBmp().
  std::string getImageBmpPath(const std::string& itemHref) const;
  bool generateImageBmp(const std::string& itemHref) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize,
                                bool allowEarlyStop = false) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;
  // True if the given spine index should be skipped during normal
  // sequential reading (page-turning, resuming, or picking the opening
  // chapter) rather than treated as a real chapter -- the EPUB3 nav
  // document, or anything else the spine marks linear="no" (footnotes, a
  // teaser page, etc.). Checks the spine's own linear="no" attribute first
  // (the actual spec mechanism), then falls back to identifying the nav
  // document specifically via the manifest's properties="nav" marker or a
  // "nav.xhtml"/"nav.html" filename match, for books that omit linear="no"
  // and the nav property alike. See getSpineIndexForTextReference()'s own
  // reasoning for why this matters: a real spine item in some books, but
  // its content is a link list, not a chapter.
  bool isNonLinearSpineIndex(int spineIndex) const;

  // Reader-choice cache -- see the ReaderChoice enum above. Cleared along
  // with the rest of this book's cache by clearCache() / "Clear Reading
  // Cache" in Settings, which doubles as the way to make ReaderActivity ask
  // again for a book that was answered before.
  ReaderChoice getSavedReaderChoice() const;
  void saveReaderChoice(ReaderChoice choice) const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
