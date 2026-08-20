#pragma once
#include <Epub.h>
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <memory>
#include <string>
#include <vector>

#include "CjkChapterFileReader.h"
#include "CjkVerticalLayout.h"
#include "activities/Activity.h"

// v1 CJK vertical (tategaki) reader engine. Opt-in via CjkReaderSettings;
// ReaderActivity routes here instead of EpubReaderActivity only when
// enabled and a font family is configured. See add_CJKreader.md.
//
// Deliberately does NOT reuse lib/Epub's Section/Page/incremental-build
// pipeline the standard reader uses -- that machinery exists to solve
// disk-cached, resumable, background-built pagination for arbitrarily large
// single-spine books, and has its own bookmark/footnote/section-cache
// support this engine doesn't replicate (known v1 limitations, not
// oversights). Chapter text itself, though, is NOT kept RAM-resident:
// on-device testing found real chapters whose extracted plain text,
// combined with the zip/inflate stream's own ~44KB decompressor overhead,
// exceeded available heap outright (not a fragmentation/allocation-strategy
// problem -- confirmed via free-heap, not just largest-block, readings at
// the failure point). Extracted text is written to and read back from a
// small per-book SD file (CjkChapterFileReader) instead, keeping this
// engine's own RAM usage independent of chapter size.
class CjkVerticalReaderActivity final : public Activity {
  // shared_ptr, not unique_ptr: passed by copy to EpubReaderChapterSelectionActivity
  // (reused as-is for the TOC screen -- its UI is a plain TOC list with no
  // horizontal-reading-specific logic, so writing a near-duplicate for this
  // reader would just be the same ~130 lines twice) while this activity
  // keeps its own ownership.
  std::shared_ptr<Epub> epub;
  SdCardFontRegistry fontRegistry;
  SdCardFontManager fontManager;
  int fontId = 0;
  // A same-family font loaded one size tier up from fontId, used for
  // <h1>-<h6> text (see CjkChapterText.h/CjkVerticalLayout.h's style
  // sentinels) -- all six heading levels collapse to this one larger size,
  // not six different sizes. 0 if the family has no larger size installed
  // (falls back to fontId, i.e. headings just don't visually stand out --
  // graceful, not a load failure). Loaded additively via
  // loadFamilyExtraSize(), same mechanism SdCardFontSystem's own UI-fallback
  // sizes use, so it doesn't disturb fontId's own slot.
  int headingFontId = 0;
  // A same-family font loaded at (the nearest available size to) the
  // built-in status bar's SMALL_FONT_ID point size, registered via
  // renderer.setFallbackFont(SMALL_FONT_ID, titleFallbackFontId) so
  // GUI.drawStatusBar()'s book-title text can render CJK the same way the
  // TOC's titles do. Unlike the TOC (many distinct chapter titles hammering
  // SdCardFont's 8-slot on-demand overflow ring -- see standardFontUnloaded's
  // sibling comment on why that swap isn't used for the whole session), this
  // is a single, unchanging string re-prewarmed once in loadReaderFonts(), so
  // keeping it registered for the full reading session is cheap and safe.
  // 0 if the family has no size close enough on disk -- the title just shows
  // boxes in the footer then, same graceful fallback headingFontId uses.
  int titleFallbackFontId = 0;
  // True once sdFontSystem.unload() has been called this session, regardless
  // of whether the subsequent load of our own font then succeeded -- onExit()
  // uses this (not overall success) to decide whether the standard reader's
  // SD font needs restoring, so a load failure partway through can't leave
  // it stuck unloaded.
  bool standardFontUnloaded = false;

  int currentSpineIndex = 0;
  // File-backed, not RAM-resident: see the class comment above and
  // CjkChapterText.h for why. Points at chapterTextPath, written fresh by
  // loadChapter() on each chapter load.
  CjkChapterFileReader chapterText;
  // Fixed path (per book, not per chapter -- only one chapter is ever
  // "current" at a time) for the extracted-plain-text SD file chapterText
  // reads from. Set once in onEnter() from epub->getCachePath(), which is
  // already guaranteed to exist by the time onEnter() runs (Epub::load()
  // calls setupCacheDir()).
  std::string chapterTextPath;
  std::vector<size_t> pageIndex;  // byte offsets; pageIndex[i]..pageIndex[i+1] is page i
  // Text level (0=normal, 1=heading -- see CjkVerticalLayout::styleFromSentinel)
  // active at the start of each page, parallel to pageIndex (one entry per
  // page, not per boundary). Lets renderPage() start a page already knowing
  // whether it opens mid-heading, without rescanning from the chapter start.
  std::vector<uint8_t> pageStartStyle;
  // Per-page image state, parallel to pageIndex (one entry per page): -1
  // for an ordinary text page, or an index into chapterImages for a page
  // that's a dedicated full-page <img> break (see
  // CjkVerticalLayout::buildPageIndex()'s outPageImageIndex parameter).
  // chapterImages holds each image's resolved href, in extraction order --
  // see CjkChapterText::ChapterTextExtractor::getImagePaths()'s comment.
  std::vector<int> pageImageIndex;
  std::vector<std::string> chapterImages;
  int currentPage = 0;
  // Set by loadChapter() on failure (href + byte size it was reading, or
  // "no href"/"not readable" for the failure before a read is even
  // attempted) so callers can put an actual reason on screen -- there's no
  // serial-log access on real-device testing, and "heap free/max-alloc"
  // alone turned out not to explain a real, content-specific failure.
  std::string lastLoadErrorDetail;
  int pagesUntilFullRefresh = 0;
  bool initialized = false;
  bool loadFailed = false;
  // Set alongside loadFailed so render() can show which specific step
  // failed -- there's no serial-log access once this is on a real device,
  // so the on-screen message has to carry the diagnosis.
  std::string loadFailMessage;

  // Oriented viewport, computed once in onEnter().
  int viewportTopY = 0;
  int viewportLeftX = 0;
  int viewportRightX = 0;
  int viewportBottomY = 0;

  // Fixed-pitch cell size derived from the loaded font -- no kerning, no
  // per-glyph advance lookups (see add_CJKreader.md: CJK bitmap glyphs are
  // fixed-pitch, so a constant step is correct and cheaper).
  int glyphAdvancePx = 0;  // vertical step per character within a column
  int columnWidthPx = 0;   // horizontal step per column
  CjkVerticalLayout::PageMetrics metrics{};

  // Shown once per book open (see onEnter()), dismissed by any page-turn/
  // confirm input in loop(). Reuses the same cover-generation/path
  // machinery the file browser, home screen, and sleep screen already use
  // (Epub::generateCoverBmp/getCoverBmpPath), not a new mechanism.
  bool showingCover = false;
  std::string coverBmpPath;

  // Loads fontId and headingFontId from CJK_READER_SETTINGS.fontFamilyName
  // together, as one unit -- both onEnter() and openChapterSelection()'s
  // result handler need to (re)do this after unloading fonts for another
  // purpose (the standard reader's SD font slot, or sdFontSystem's TOC
  // fallback), and having two separate call sites each load "the fonts"
  // by hand is exactly how headingFontId ended up forgotten in one of them
  // on a real device: the TOC path only restored fontId, leaving
  // headingFontId pointing at a font unloadAll() had already deleted, so
  // headings silently stopped drawing for the rest of the session after
  // the first TOC visit. Returns false (and sets loadFailed) only if the
  // main reading font fails -- a missing heading size is not fatal (see
  // headingFontId's own comment).
  bool loadReaderFonts();

  bool loadChapter(int spineIndex);
  void renderPage() const;
  void renderCoverPage() const;
  // Shared by renderCoverPage() (a fixed, well-known bitmap) and the
  // current page's image break, if any (an arbitrary in-book image at
  // imageBmpPath) -- same centered scale-to-fit placement and 3-pass
  // grayscale rendering sequence either way.
  void renderFullPageBitmap(const std::string& bmpPath) const;
  void renderStatusBar() const;
  void openChapterSelection();

  // Reading-position persistence. Stored as (spineIndex, byte offset into
  // that chapter's extracted plain text) rather than a page index: page
  // boundaries depend on viewport/font/column settings, which can change
  // between sessions, while a byte offset into the chapter's own text stays
  // valid regardless -- loadProgress() just has to find which page currently
  // contains it. Own file (not ProgressFile::writeAtomic's progress.bin):
  // that name is the standard reader's, in the same cache dir, in a
  // different format -- reusing it would let switching between engines on
  // the same book corrupt each other's saved position.
  struct Progress {
    int spineIndex = 0;
    uint32_t byteOffset = 0;
  };
  bool loadProgress(Progress& out) const;
  void saveProgress() const;
  // Sets currentPage to whichever page contains byteOffset in the chapter
  // pageIndex currently describes. Called after loadChapter() rebuilds
  // pageIndex for the resumed spineIndex.
  void seekToByteOffset(uint32_t byteOffset);

 public:
  explicit CjkVerticalReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     std::unique_ptr<Epub> epub, int initialRefreshCountdown)
      : Activity("CjkVerticalReader", renderer, mappedInput),
        epub(std::move(epub)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
