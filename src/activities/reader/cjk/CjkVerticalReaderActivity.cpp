#include "CjkVerticalReaderActivity.h"

#include <Bitmap.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>

#include "CjkChapterText.h"
#include "CjkKinsoku.h"
#include "CjkReaderSettings.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/reader/EpubReaderChapterSelectionActivity.h"
#include "activities/reader/ProgressFile.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
int columnSpacingPx(const uint8_t spacing) {
  switch (spacing) {
    case CjkReaderSettings::SPACING_SMALL:
      return 4;
    case CjkReaderSettings::SPACING_LARGE:
      return 18;
    case CjkReaderSettings::SPACING_MEDIUM:
    default:
      return 10;
  }
}

// Feeds chapter bytes into a ChapterTextExtractor as they stream in from
// Epub::readItemContentsToStream, so loadChapter() never needs the whole
// chapter's raw HTML in one contiguous buffer.
class ChapterTextStreamSink final : public Print {
  CjkChapterText::ChapterTextExtractor& extractor;

 public:
  explicit ChapterTextStreamSink(CjkChapterText::ChapterTextExtractor& extractor) : extractor(extractor) {}
  size_t write(const uint8_t b) override {
    const char c = static_cast<char>(b);
    extractor.feed(&c, 1);
    return 1;
  }
  size_t write(const uint8_t* buffer, const size_t size) override {
    extractor.feed(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};
}  // namespace

void CjkVerticalReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    finish();
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Fixed per-book path for the extracted-plain-text file loadChapter()
  // writes and chapterText reads back from -- see the class comment for
  // why chapter text lives on SD, not in RAM. epub->getCachePath() is
  // guaranteed to exist already: Epub::load() calls setupCacheDir().
  chapterTextPath = epub->getCachePath() + "/cjk_chapter_text.bin";

  const auto path = epub->getPath();
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(path, epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  if (CJK_READER_SETTINGS.fontFamilyName.empty()) {
    LOG_ERR("CJKR", "CJK reader enabled with no font family configured");
    loadFailed = true;
    loadFailMessage = tr(STR_CJK_NO_FONT_SET);
    requestUpdate();
    return;
  }

  fontRegistry.discover();

  // ReaderActivity::onEnter() unconditionally loads the standard reader's
  // SD font (sdFontSystem.ensureLoaded()) before dispatching here. Font IDs
  // are content-hash based and global to the renderer, so if this is the
  // same family+size the standard reader is already using, loading it again
  // under a second SdCardFontManager fails as an ID collision rather than
  // succeeding. Free that slot first -- restored in onExit().
  sdFontSystem.unload(renderer);
  standardFontUnloaded = true;

  if (!loadReaderFonts()) {
    requestUpdate();
    return;
  }

  // Deliberately NOT registering our font as the UI CJK fallback here
  // anymore (an earlier round did). It fixed CJK titles showing as blank in
  // the TOC list, but two separate on-device crash reports (abort(), full
  // reboot) traced back to the exact same trigger: that fallback routes
  // built-in-UI-font text through SdCardFont's 8-slot on-demand overflow
  // ring, and a TOC screen with many unique CJK title characters bursts
  // through it far harder than normal reading ever does -- a ~4-second
  // render followed by abort() both times, even after prewarming the TOC
  // text (see openChapterSelection()). That's a pre-existing fragility in
  // shared font-fallback code, not something to keep patching blind from
  // here. Reverting to no fallback: CJK-titled TOC entries show boxes
  // again, but the reader doesn't reboot the device.

  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const uint8_t screenMargin = SETTINGS.screenMargin;
  viewportTopY = marginTop + screenMargin;
  viewportLeftX = marginLeft + screenMargin;
  viewportRightX = renderer.getScreenWidth() - marginRight - screenMargin;
  viewportBottomY = renderer.getScreenHeight() - marginBottom -
                    std::max<int>(screenMargin, UITheme::getInstance().getStatusBarHeight());

  // Compressed step: an earlier attempt at this (0.82x) caused a real
  // on-device crash, but that was before any margin logic existed to
  // account for it -- glyphs were drawn at full size into a shrunk grid
  // with zero protection against the resulting overhang. This time the
  // safety margins below are computed against trueLineHeight (the font's
  // actual, uncompressed size -- i.e. the real glyph size being drawn),
  // not the compressed step, so they stay just as protective regardless of
  // how tight kLineCompression is set. 0.90 is a deliberately modest
  // starting point (short of the 0.82 that caused problems) -- visual
  // tightness is a judgment call to dial in from here, crash-safety is not.
  const int trueLineHeight = std::max(renderer.getLineHeight(fontId), 1);
  constexpr float kLineCompression = 0.90f;
  glyphAdvancePx = std::max(static_cast<int>(trueLineHeight * kLineCompression + 0.5f), 1);
  columnWidthPx = glyphAdvancePx + columnSpacingPx(CJK_READER_SETTINGS.columnSpacing);

  // Reserve room for headingFontId's taller glyphs at the bottom of the
  // viewport: a heading character can land on a column's last row (the
  // fixed grid step doesn't grow for it -- see headingFontId's own
  // "same grid step, some overflow" comment), and without this margin that
  // overflow lands past viewportBottomY, overlapping the page-number status
  // bar below it. Reserving the size difference up front instead means the
  // grid itself has one fewer row of slack rather than the heading glyph
  // overflowing into UI chrome. 0 if headingFontId isn't loaded, or isn't
  // actually taller than the (uncompressed) reading font size.
  const int headingExtraHeight =
      headingFontId != 0 ? std::max(0, renderer.getLineHeight(headingFontId) - trueLineHeight) : 0;

  // Ordinary (non-heading) rows also have no guarantee against bleeding
  // into the status bar: a glyph's actual rendered bitmap can exceed its
  // nominal cell (confirmed on-device: an "Outside range"
  // draw-past-the-screen-edge abort() was reproduced this way). Each row's
  // Y position comes straight from row*glyphAdvancePx (not accumulated
  // from previous rows' actual drawn extent), so compression doesn't
  // compound down the column -- every row overhangs its own cell by the
  // same fixed (trueLineHeight - glyphAdvancePx), independent of row
  // index. What matters for crash-safety is only the last row's absolute
  // bottom edge, so reserving that one overhang amount once (not per row)
  // is what the math below actually needs -- verified algebraically, not
  // guessed: with this margin, (rowsPerColumn-1)*glyphAdvancePx +
  // trueLineHeight <= viewportBottomY - viewportTopY always holds. A full
  // row of uncompressed slack (confirmed on-device to fully stop the
  // overflow at 1.0x) left more empty space above the footer than wanted;
  // settled on 2/3 of a trueLineHeight row as the residual per-glyph-
  // overshoot margin (same reasoning as before compression existed), plus
  // this compression overhang on top.
  const int compressionOverhangPerRow = std::max(0, trueLineHeight - glyphAdvancePx);
  const int rowSafetyMarginPx = trueLineHeight * 2 / 3 + compressionOverhangPerRow;
  metrics.rowsPerColumn =
      std::max(1, (viewportBottomY - headingExtraHeight - rowSafetyMarginPx - viewportTopY) / glyphAdvancePx);
  metrics.columnsPerPage = std::max(1, (viewportRightX - viewportLeftX) / columnWidthPx);

  Progress resume;
  const bool hasResume = loadProgress(resume);

  // Shown once, only on a genuinely fresh open (never on resume -- picking
  // back up mid-book shouldn't interrupt every session with the cover
  // again). generateCoverBmp() is a no-op if already cached; a failure
  // here (e.g. no cover art in this EPUB) just skips straight to the first
  // chapter, same graceful fallback SleepActivity's cover rendering
  // already uses.
  if (!hasResume && epub->generateCoverBmp()) {
    coverBmpPath = epub->getCoverBmpPath();
    showingCover = true;
  }

  // No saved position: start at the EPUB's declared text-reference spine
  // item, not spine[0]. Spine[0] is very often a cover or nav/TOC document,
  // not the first real chapter -- without this a fresh open renders the
  // nav document's own link list as if it were chapter body text. Mirrors
  // EpubReaderActivity's exact same fallback for the same reason.
  //
  // A saved position can also point at a non-linear item (the nav
  // document, or anything else spine-marked linear="no"), if it was saved
  // before isNonLinearSpineIndex() existed to steer opens away from it --
  // self-correct rather than resuming into it, since its byte offset was
  // into that item's own text, not real chapter content worth seeking
  // back into.
  const bool resumeIsNav = hasResume && epub->isNonLinearSpineIndex(resume.spineIndex);
  const int startSpine = (hasResume && !resumeIsNav) ? resume.spineIndex : epub->getSpineIndexForTextReference();
  if (loadChapter(startSpine)) {
    if (hasResume && !resumeIsNav) seekToByteOffset(resume.byteOffset);
  } else if (startSpine != 0 && loadChapter(0)) {
    // Saved chapter no longer resolves (different/edited book sharing this
    // cache dir) -- start over from the beginning rather than fail outright.
  } else {
    loadFailed = true;
  }

  initialized = true;
  requestUpdate();
}

bool CjkVerticalReaderActivity::loadReaderFonts() {
  const auto* family = fontRegistry.findFamily(CJK_READER_SETTINGS.fontFamilyName);
  if (!family) {
    LOG_ERR("CJKR", "CJK font family '%s' not found on SD card (registry has %d families)",
            CJK_READER_SETTINGS.fontFamilyName.c_str(), fontRegistry.getFamilyCount());
    loadFailed = true;
    loadFailMessage = tr(STR_CJK_FONT_NOT_FOUND);
    return false;
  }

  if (!fontManager.loadFamily(*family, renderer, CJK_READER_SETTINGS.fontPointSize)) {
    LOG_ERR("CJKR", "Failed to load CJK font family '%s' at %u pt", CJK_READER_SETTINGS.fontFamilyName.c_str(),
            CJK_READER_SETTINGS.fontPointSize);
    loadFailed = true;
    loadFailMessage = tr(STR_CJK_FONT_LOAD_ERROR);
    return false;
  }
  fontId = fontManager.getFontId(CJK_READER_SETTINGS.fontFamilyName);

  // Additively load one size tier up from the reading size, for <h1>-<h6>
  // headings (see the header's headingFontId comment). loadFamilyExtraSize
  // needs an exact installed size, so find the smallest one actually larger
  // than the reading size rather than guessing a percentage bump that might
  // not exist on disk; 0 (fontManager's own "not found" sentinel) if the
  // family only ships the one size already loaded -- headings then just
  // don't visually stand out, not a load failure.
  uint8_t headingPointSize = CJK_READER_SETTINGS.fontPointSize;
  for (const uint8_t sz : family->availableSizes()) {
    if (sz > CJK_READER_SETTINGS.fontPointSize &&
        (headingPointSize == CJK_READER_SETTINGS.fontPointSize || sz < headingPointSize)) {
      headingPointSize = sz;
    }
  }
  headingFontId = headingPointSize > CJK_READER_SETTINGS.fontPointSize
                     ? fontManager.loadFamilyExtraSize(*family, renderer, headingPointSize)
                     : 0;

  // Additively load the size closest to SMALL_FONT_ID's own (8pt -- see
  // SdCardFontSystem's kUiFontSizes) so the status bar's book-title text
  // (drawn with SMALL_FONT_ID, see BaseTheme::drawStatusBar) can redirect to
  // real CJK glyphs instead of boxes. loadFamilyExtraSize needs an exact
  // installed size, same constraint headingPointSize's search works around.
  uint8_t titlePointSize = 0;
  for (const uint8_t sz : family->availableSizes()) {
    if (titlePointSize == 0 || std::abs(static_cast<int>(sz) - 8) < std::abs(static_cast<int>(titlePointSize) - 8)) {
      titlePointSize = sz;
    }
  }
  titleFallbackFontId = titlePointSize != 0 ? fontManager.loadFamilyExtraSize(*family, renderer, titlePointSize) : 0;
  if (titleFallbackFontId != 0) {
    renderer.setFallbackFont(SMALL_FONT_ID, titleFallbackFontId);
    // One short, unchanging string -- prewarm once so the per-page status
    // bar draw never triggers an on-demand SD glyph load.
    if (epub) renderer.ensureSdCardFontReady(titleFallbackFontId, epub->getTitle().c_str());
  }

  return true;
}

void CjkVerticalReaderActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  fontManager.unloadAll(renderer);
  fontId = 0;
  headingFontId = 0;
  titleFallbackFontId = 0;
  // Restore the standard reader's SD font if we unloaded it (see onEnter):
  // other screens (file browser, home) use it for CJK book-title fallback
  // rendering, and it would otherwise stay unloaded until the next book open.
  if (standardFontUnloaded) sdFontSystem.ensureLoaded(renderer);
  chapterText = CjkChapterFileReader();  // closes the underlying file handle
  pageIndex.clear();
  pageStartStyle.clear();
  pageImageIndex.clear();
  chapterImages.clear();
  // Clears the crash-loop guard main.cpp sets on boot-resume: reaching a
  // clean onExit() means this session didn't crash mid-read. Every other
  // reader activity clears this the same way.
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  epub.reset();
}

bool CjkVerticalReaderActivity::loadChapter(const int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    lastLoadErrorDetail = "invalid spine index";
    return false;
  }

  // Wait for any in-progress render to fully finish before touching the SD
  // card at all -- purely a synchronization wait, released immediately, not
  // held across the I/O below (that would violate
  // docs/activity-manager.md's "don't hold RenderLock across blocking
  // calls" rule the other way). requestUpdate(false) (used throughout this
  // reader) only queues a flag; it does not wait for the render task to
  // actually run. Without this wait, the render task can still be mid-page,
  // doing its own SD reads for on-demand glyph loads (SdCardFont's overflow
  // path), while this function starts a second, concurrent SD read for the
  // next chapter on the main task. A crash report from testing this exact
  // sequence -- an overflow-heavy page still loading glyphs, "before
  // loadChapter" firing while that was in progress, an abort() shortly
  // after -- matches two tasks racing the SD card, not a single-task bug.
  { RenderLock waitForRenderIdle(*this); }

  const auto spineItem = epub->getSpineItem(spineIndex);
  if (spineItem.href.empty()) {
    lastLoadErrorDetail = "spine item has no href";
    return false;
  }

  // Close the outgoing chapter's read handle before the new chapter's write
  // overwrites the same underlying file (chapterTextPath is a single fixed
  // path -- only one chapter is ever "current"). Also drops pageIndex,
  // matching it. This costs nothing on a failed load: render() already
  // shows a dedicated error screen on failure (loadFailed, or independently
  // pageIndex.size() < 2 -- see render()'s guard), never a stale view of
  // the old chapter.
  {
    RenderLock lock(*this);
    chapterText = CjkChapterFileReader();
    pageIndex.clear();
  }

  // Streamed, not epub->readItemContentsToBytes(): that reads the whole
  // chapter's raw HTML into one contiguous buffer up front, which on-device
  // testing found real chapters can exceed even with 100+ KB of free heap.
  // ChapterTextExtractor writes its output straight to chapterTextPath
  // through a small fixed-size buffer instead of holding any meaningful
  // amount of the chapter's text in RAM -- see CjkChapterText.h and the
  // class comment above for why: a large chapter's extracted text,
  // combined with the zip/inflate stream's own fixed ~44KB decompressor
  // overhead (state+window), was found to exceed total available heap
  // outright on real chapters, not just fragment it. Writing to disk
  // instead makes this independent of chapter size.
  const std::string chapterBaseDir = spineItem.href.substr(0, spineItem.href.find_last_of('/') + 1);
  CjkChapterText::ChapterTextExtractor extractor;
  if (!extractor.beginWrite(chapterTextPath, chapterBaseDir)) {
    lastLoadErrorDetail = "failed to open chapter text file for writing";
    LOG_ERR("CJKR", "%s: %s", lastLoadErrorDetail.c_str(), chapterTextPath.c_str());
    return false;
  }
  ChapterTextStreamSink sink(extractor);
  constexpr size_t kStreamChunkSize = 4096;
  if (!epub->readItemContentsToStream(spineItem.href, sink, kStreamChunkSize, false)) {
    char detail[80];
    snprintf(detail, sizeof(detail), "readItemContentsToStream failed: %s", spineItem.href.c_str());
    lastLoadErrorDetail = detail;
    LOG_ERR("CJKR", "Failed to stream chapter %d (%s)", spineIndex, spineItem.href.c_str());
    return false;
  }
  const size_t bytesWritten = extractor.finish();
  if (extractor.ioError()) {
    char detail[96];
    snprintf(detail, sizeof(detail), "SD write failed extracting chapter text (%s)", spineItem.href.c_str());
    lastLoadErrorDetail = detail;
    LOG_ERR("CJKR", "%s", detail);
    return false;
  }

  // Reopen for reading -- writing and reading the same file through one
  // handle isn't something HalFile's write-buffered Print interface
  // supports cleanly, and this also guarantees everything the extractor
  // wrote is actually flushed and visible (finish() closed the write
  // handle above) before pagination reads it back.
  CjkChapterFileReader newChapterText;
  if (!newChapterText.open(chapterTextPath) || newChapterText.size() != bytesWritten) {
    lastLoadErrorDetail = "failed to reopen chapter text file for reading";
    LOG_ERR("CJKR", "%s: %s", lastLoadErrorDetail.c_str(), chapterTextPath.c_str());
    return false;
  }

  // NOT prewarming the whole chapter here (the original plan's "prewarm per
  // chapter, not per page" idea) -- SdCardFont's mini glyph cache is
  // deliberately "kept, not freed" across prewarms so repeat pages don't
  // re-pay SD reads, which means prewarming a full chapter's unique-glyph
  // set makes that resident cache grow to fit the LARGEST chapter seen this
  // session and never shrinks back down on its own. renderPage() prewarms
  // one page's text at a time instead -- a bounded, small working set.
  std::vector<int> newPageImageIndex;
  std::vector<size_t> newPageIndex = CjkVerticalLayout::buildPageIndex(
      newChapterText, metrics, CJK_READER_SETTINGS.kinsokuEnabled, &newPageImageIndex);
  // Heading (vs normal) text level active at the start of each page -- see
  // the header's pageStartStyle comment for why this is a separate pass
  // rather than threaded through buildPageIndex's own scan.
  std::vector<uint8_t> newPageStartStyle = CjkVerticalLayout::computePageStartStyles(newChapterText, newPageIndex);
  // Resolved <img> hrefs found during extraction, in extraction order --
  // pageImageIndex's per-page values index into this. See
  // CjkChapterText::ChapterTextExtractor::getImagePaths()'s own comment.
  std::vector<std::string> newChapterImages = extractor.getImagePaths();

  // Commit under a RenderLock. ActivityManager runs render() on a separate
  // task from loop()/result handlers, and this reader has no synchronization
  // on chapterText/pageIndex/currentPage/currentSpineIndex beyond this lock
  // -- an unprotected reassignment here is exactly the "modifying shared
  // state without RenderLock" pitfall docs/activity-manager.md calls out:
  // the render task can read a std::vector mid-reassignment, which is
  // undefined behavior. That was the actual cause of a real on-device
  // abort()/reboot selecting a chapter from the TOC screen -- confirmed by
  // resolving the panic PC against the local build with addr2line.
  {
    RenderLock lock(*this);
    chapterText = std::move(newChapterText);
    pageIndex = std::move(newPageIndex);
    pageStartStyle = std::move(newPageStartStyle);
    pageImageIndex = std::move(newPageImageIndex);
    chapterImages = std::move(newChapterImages);
    currentSpineIndex = spineIndex;
    currentPage = 0;
  }
  return true;
}

void CjkVerticalReaderActivity::seekToByteOffset(const uint32_t byteOffset) {
  for (size_t i = 0; i + 1 < pageIndex.size(); i++) {
    if (byteOffset >= pageIndex[i] && byteOffset < pageIndex[i + 1]) {
      currentPage = static_cast<int>(i);
      return;
    }
  }
  // Past the end of a rebuilt/shorter pageIndex (e.g. font/column settings
  // changed since the offset was saved) -- land on the last page rather
  // than an out-of-range one.
  if (pageIndex.size() >= 2) currentPage = static_cast<int>(pageIndex.size()) - 2;
}

bool CjkVerticalReaderActivity::loadProgress(Progress& out) const {
  if (!epub) return false;
  HalFile f;
  if (!Storage.openFileForRead("CJKR", epub->getCachePath() + "/cjk_progress.bin", f)) return false;
  uint8_t data[8];
  if (f.read(data, sizeof(data)) != sizeof(data)) return false;
  out.spineIndex = static_cast<int>(static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                                    (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24));
  out.byteOffset = static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8) |
                   (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 24);
  return true;
}

void CjkVerticalReaderActivity::saveProgress() const {
  if (!epub || pageIndex.empty() || currentPage < 0 || static_cast<size_t>(currentPage) >= pageIndex.size()) return;

  const auto spine = static_cast<uint32_t>(currentSpineIndex);
  const auto byteOffset = static_cast<uint32_t>(pageIndex[currentPage]);
  uint8_t data[8];
  data[0] = static_cast<uint8_t>(spine);
  data[1] = static_cast<uint8_t>(spine >> 8);
  data[2] = static_cast<uint8_t>(spine >> 16);
  data[3] = static_cast<uint8_t>(spine >> 24);
  data[4] = static_cast<uint8_t>(byteOffset);
  data[5] = static_cast<uint8_t>(byteOffset >> 8);
  data[6] = static_cast<uint8_t>(byteOffset >> 16);
  data[7] = static_cast<uint8_t>(byteOffset >> 24);

  if (!ProgressFile::writeAtomic(epub->getCachePath(), data, sizeof(data), "cjk_progress.bin")) {
    LOG_ERR("CJKR", "Failed to save CJK reading progress");
  }
}

// Heap logging kept at each stage from when this crashed three times
// earlier this session (two causes fixed and confirmed; a third,
// __cxxabiv1::__unexpected with an unclear trigger from a raw/unsymbolized
// stack dump, was never pinned down) -- useful if a crash resurfaces here.
void CjkVerticalReaderActivity::openChapterSelection() {
  const int spineAtOpen = currentSpineIndex;
  const std::string path = epub->getPath();

  LOG_DBG("CJKR", "openChapterSelection: heap free=%u max-alloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  // Swap this reader's own font out for the standard app-wide sdFontSystem
  // while the TOC screen is up, so chapter titles render through the same,
  // already-proven CJK-fallback mechanism the file browser/home screen use
  // (SdCardFontSystem::setupUiFallbacks -- size-matched fallbacks for
  // UI_10_FONT_ID etc., probed for real CJK coverage before registering).
  // An earlier attempt registered this reader's own font as an ad-hoc
  // fallback directly and hit an unresolved crash rendering many unique
  // title glyphs; reusing the mechanism the rest of the app already relies
  // on for the exact same job (CJK text in a Latin-UI list row) is a much
  // better-tested path than a second, bespoke one.
  { RenderLock waitForRenderIdle(*this); }
  fontManager.unloadAll(renderer);
  fontId = 0;
  headingFontId = 0;
  sdFontSystem.ensureLoaded(renderer);

  // Prewarm every chapter title's glyphs into the fallback SD font's mini
  // cache in one batch, not just the currently-visible page: a TOC's total
  // text is small (a few KB at most across a whole book, unlike chapter
  // body text) so there's no memory reason to prewarm incrementally, and
  // doing it once up front means scrolling through the list never touches
  // SD again afterward. This also sidesteps the mechanism behind the
  // earlier unresolved TOC crash: prewarmed glyphs land in the font's
  // larger mini-cache, not the 8-slot on-demand overflow ring that burst
  // under many unique characters rendering directly. Scan pass (draws
  // nothing, just records text -- see GfxRenderer::isFontCacheScanning)
  // then a real prewarm, the same two-step pattern EpubReaderActivity uses
  // for page text.
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    const int tocCount = epub->getTocItemsCount();
    for (int i = 0; i < tocCount; i++) {
      renderer.drawText(UI_10_FONT_ID, 0, 0, epub->getTocItem(i).title.c_str());
    }
    scope.endScanAndPrewarm();
  }
  // Verifiable, not just assumed: this and the matching log right after
  // sdFontSystem.unload() in the result handler below should show heap
  // free landing back close to this pre-prewarm number, confirming the
  // prewarmed data (which lives inside the SdCardFont objects
  // sdFontSystem.unload() deletes -- see SdCardFont::freeAll(), called
  // from its destructor) doesn't linger once the TOC screen closes.
  LOG_DBG("CJKR", "after TOC prewarm: heap free=%u max-alloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  startActivityForResult(
      std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineAtOpen),
      [this](const ActivityResult& result) {
        // Restore this reader's own font before anything else below --
        // needed for rendering to resume correctly whether the user picked
        // a chapter or cancelled, and loadChapter() (if reached) needs
        // sdFontSystem's slot free again for the same font-ID-collision
        // reason onEnter() unloads it in the first place.
        sdFontSystem.unload(renderer);
        // Compare against "after TOC prewarm" above: should land back close
        // to that pre-prewarm number, confirming sdFontSystem.unload()
        // actually released the prewarmed TOC-title glyph data (deletes the
        // SdCardFont objects it loaded -- see SdCardFontManager::unloadAll)
        // rather than it lingering.
        LOG_DBG("CJKR", "after sdFontSystem.unload(): heap free=%u max-alloc=%u",
                static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
        // loadReaderFonts(), not a hand-rolled reload: this is exactly the
        // call site where headingFontId was previously forgotten (only
        // fontId was restored here), leaving it dangling after unloadAll()
        // above -- see loadReaderFonts()'s own comment.
        if (!loadReaderFonts()) {
          LOG_ERR("CJKR", "Failed to reload CJK reader fonts after chapter selection");
        }

        LOG_DBG("CJKR", "chapter selection result handler entered: heap free=%u max-alloc=%u",
                static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        // get_if, not get: this firmware builds with -fno-exceptions, so a
        // std::get mismatch (result.data not actually holding ChapterResult)
        // would call std::terminate() and abort instead of throwing --
        // exactly the kind of "select a chapter and it crashes" symptom
        // that's silent without a serial log. Fail soft instead.
        const auto* chapterResult = std::get_if<ChapterResult>(&result.data);
        if (!chapterResult) {
          LOG_ERR("CJKR", "Chapter selection returned an unexpected result type");
          requestUpdate();
          return;
        }
        const int targetSpine = chapterResult->spineIndex;
        LOG_DBG("CJKR", "before loadChapter(%d): heap free=%u max-alloc=%u", targetSpine,
                static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
        // loadChapter() takes its own RenderLock internally, only around the
        // final state commit -- the SD read and string/vector build happen
        // unlocked before that, so nothing extra is needed at this call site.
        if (targetSpine != currentSpineIndex && !loadChapter(targetSpine)) {
          char buf[160];
          snprintf(buf, sizeof(buf), "Chapter load failed: spine %d->%d (%s)", currentSpineIndex, targetSpine,
                   lastLoadErrorDetail.c_str());
          LOG_ERR("CJKR", "%s", buf);
          RenderLock lock(*this);
          loadFailed = true;
          loadFailMessage = buf;
        }
        LOG_DBG("CJKR", "after loadChapter: heap free=%u max-alloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
        requestUpdate();
      });
}

void CjkVerticalReaderActivity::loop() {
  if (loadFailed) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      onGoHome();
    }
    return;
  }

  if (ReaderUtils::handleBackNavigation(
          mappedInput, activityManager, epub ? epub->getPath().c_str() : "",
          {this, [](void* ctx) { static_cast<CjkVerticalReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  if (showingCover) {
    // Dismiss on any page-turn or Confirm input -- Back already exited to
    // home above. Doesn't advance a page or open the TOC: the cover isn't
    // "page 0", it's a one-time intro the first real input clears.
    const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
    auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
    if (prevTriggered || touch.prev || nextTriggered || touch.next ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      showingCover = false;
      requestUpdate();
    }
    return;
  }

  // Confirm -> openChapterSelection() was deliberately disconnected earlier
  // this session after TOC selection crashed the device on three separate
  // underlying causes: a RenderLock data race (fixed, confirmed), a
  // large-chapter allocation failure (fixed -- and since chapterText moved
  // off RAM entirely onto SD, the whole class of "chapter load exhausts
  // heap" failure this was part of no longer applies to loadChapter(),
  // which openChapterSelection()'s result handler also calls), and one
  // unresolved case (__cxxabiv1::__unexpected, unclear exact trigger from a
  // raw/unsymbolized stack dump). Reconnecting now that the memory-pressure
  // causes are structurally gone, not just patched -- see
  // openChapterSelection()'s own comments for the heap logging still in
  // place if the unresolved case resurfaces.
  //
  // wasReleased, not wasPressed: EpubReaderChapterSelectionActivity itself
  // selects on wasReleased(Confirm) (see selectChapter()'s trigger in its
  // own loop()). Opening on wasPressed means the matching release hasn't
  // happened yet when this activity switch happens -- it fires moments
  // later, once the TOC screen is already active, and gets delivered to
  // the TOC's own loop() as if the user had just released Confirm there,
  // immediately selecting whatever's highlighted (the current chapter, by
  // default) and closing it again. Opening on wasReleased instead means
  // the button event is already fully finished by the time the switch
  // happens, matching EpubReaderActivity's own Confirm -> openReaderMenu()
  // pattern for exactly this reason.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openChapterSelection();
    return;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) return;

  const int totalPages = std::max<int>(1, static_cast<int>(pageIndex.size()) - 1);

  // A failed cross-chapter loadChapter() used to leave the reader frozen on
  // the last/first page with zero feedback -- "press next, nothing happens,
  // forever". Surface it as the same visible failure state onEnter() uses
  // instead of silently doing nothing, so a persistent failure is at least
  // reportable (LOG_ERR still fires for serial-log cases; this covers the
  // "no serial access" case, which is every real-device test so far).
  const auto failToLoadChapter = [this](const int targetSpine) {
    // Put the actual diagnosis on screen, not the generic "check font"
    // message -- that message is accurate for onEnter()'s font-load
    // failures but actively misleading here (this is a mid-book chapter
    // read failure, unrelated to font setup), and there's no serial-log
    // access on real-device testing to fall back on.
    char buf[160];
    snprintf(buf, sizeof(buf), "Chapter load failed: spine %d->%d (%s), heap free=%u max-alloc=%u", currentSpineIndex,
             targetSpine, lastLoadErrorDetail.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
             static_cast<unsigned>(ESP.getMaxAllocHeap()));
    LOG_ERR("CJKR", "%s", buf);
    loadFailed = true;
    loadFailMessage = buf;
    requestUpdate();
  };

  if (prevTriggered) {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    } else if (currentSpineIndex > 0) {
      // Skip past any non-linear spine items (nav document, etc.) instead
      // of landing on one -- same reasoning as onEnter()'s opening-chapter
      // and stale-resume handling: they're real spine items in some books,
      // but their content is a link list or similar, not a chapter.
      int target = currentSpineIndex - 1;
      while (target >= 0 && epub->isNonLinearSpineIndex(target)) target--;
      if (target < 0) return;
      LOG_DBG("CJKR", "page-turn prev: before loadChapter(%d), heap free=%u max-alloc=%u", target,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      if (loadChapter(target)) {
        const int newTotalPages = std::max<int>(1, static_cast<int>(pageIndex.size()) - 1);
        currentPage = newTotalPages - 1;
        requestUpdate();
      } else {
        failToLoadChapter(target);
      }
    }
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      int target = currentSpineIndex + 1;
      const int spineCount = epub->getSpineItemsCount();
      while (target < spineCount && epub->isNonLinearSpineIndex(target)) target++;
      if (target >= spineCount) {
        onGoHome();
        return;
      }
      LOG_DBG("CJKR", "page-turn next: before loadChapter(%d), heap free=%u max-alloc=%u", target,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      if (loadChapter(target)) {
        requestUpdate();
      } else {
        failToLoadChapter(target);
      }
    } else {
      onGoHome();
    }
  }
}

void CjkVerticalReaderActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (showingCover) {
    renderCoverPage();  // handles its own display call(s) -- see its comment
    return;
  }

  // Full-page image break (see CjkVerticalLayout::buildPageIndex()'s
  // outPageImageIndex): same early-return, no-status-bar treatment as the
  // cover screen above, so an in-book illustration shows edge-to-edge
  // rather than sharing the page with reading-progress chrome.
  // generateImageBmp() is a no-op once the BMP already exists in cache
  // (same short-circuit as generateCoverBmp()), so revisiting this page is
  // cheap after the first conversion. A failure here (corrupt/unsupported
  // image) falls through to the normal text page instead -- pageImageIndex
  // still points at the image, but the loop() page-turn just moves the
  // reader forward next time same as any other page, rather than getting
  // stuck showing nothing.
  if (static_cast<size_t>(currentPage) < pageImageIndex.size() && pageImageIndex[currentPage] >= 0) {
    const std::string& imageHref = chapterImages[pageImageIndex[currentPage]];
    if (epub->generateImageBmp(imageHref)) {
      renderFullPageBitmap(epub->getImageBmpPath(imageHref));
      saveProgress();
      return;
    }
  }

  if (loadFailed || !initialized || pageIndex.size() < 2) {
    // Wrapped, multi-line -- a single drawCenteredText call doesn't wrap,
    // so the diagnostic messages built in loop() (which include live heap
    // numbers) were running off the screen edge with no indication they'd
    // been cut off.
    const std::string message = loadFailed && !loadFailMessage.empty() ? loadFailMessage : tr(STR_CJK_LOAD_FAILED);
    const int contentWidth = renderer.getScreenWidth() - 2 * UITheme::getInstance().getMetrics().contentSidePadding;
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, message.c_str(), contentWidth, 8);
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    int y = renderer.getScreenHeight() / 2 - static_cast<int>(lines.size()) * lineHeight / 2;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str());
      y += lineHeight;
    }
    renderer.displayBuffer();
    return;
  }

  renderPage();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  saveProgress();
}

void CjkVerticalReaderActivity::renderCoverPage() const { renderFullPageBitmap(coverBmpPath); }

void CjkVerticalReaderActivity::renderFullPageBitmap(const std::string& bmpPath) const {
  // Simple centered, scaled-to-fit rendering -- no crop-mode/filter options
  // like SleepActivity's cover screen, since this is a one-time intro
  // screen (or an in-book image break) inside the reader, not a persistent
  // idle display users tune.
  HalFile file;
  if (!Storage.openFileForRead("CJKR", bmpPath, file)) return;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  int x, y;
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ratio > screenRatio) {
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);

  // GfxRenderer::drawBitmap only reproduces real gray levels in
  // GRAYSCALE_MSB/LSB render mode -- in the plain BW mode this reader
  // otherwise always stays in (pure text, no prior need for grayscale),
  // its val<3 check draws every non-white dithered pixel as solid black,
  // which is what made the 2-bit-dithered cover BMP look flat and blocky
  // instead of showing real gray. This is the same three-pass base/LSB/MSB
  // sequence SleepActivity's own cover rendering already uses for exactly
  // this reason.
  if (bitmap.hasGreyscale()) {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  } else {
    renderer.displayBuffer();
  }
}

void CjkVerticalReaderActivity::renderPage() const {
  const size_t pageStart = pageIndex[currentPage];
  const size_t pageEnd = pageIndex[currentPage + 1];

  // Prewarm just this page's glyphs (see loadChapter()'s comment on why not
  // the whole chapter). Cheap even on a revisit: SdCardFont keeps resident
  // data it already has, so this is a fast no-op SD-wise for pages already
  // seen this session. Prewarms headingFontId too (harmless no-op if this
  // page has no heading text) -- it's a distinct SdCardFont instance with
  // its own advance-table cache.
  if (fontId != 0 && pageEnd > pageStart) {
    std::string pageText = CjkVerticalLayout::extractRange(chapterText, pageStart, pageEnd);
    // Also prewarm each rotated-punctuation codepoint's Vertical Forms
    // substitute (see CjkKinsoku::verticalFormFor): the draw loop may
    // switch to it instead of rotating, and it won't otherwise appear
    // anywhere in pageText for this pass to find on its own. A no-op
    // appendix if the loaded font doesn't have that range at all.
    const unsigned char* scan = reinterpret_cast<const unsigned char*>(pageText.data());
    const unsigned char* scanEnd = scan + pageText.size();
    std::string extra;
    while (scan < scanEnd) {
      const unsigned char* before = scan;
      const uint32_t cp = utf8NextCodepoint(&scan);
      if (scan == before) break;
      const uint32_t vcp = CjkKinsoku::verticalFormFor(cp);
      if (vcp != 0) utf8AppendCodepoint(vcp, extra);
    }
    pageText += extra;
    renderer.ensureSdCardFontReady(fontId, pageText.c_str());
    if (headingFontId != 0) renderer.ensureSdCardFontReady(headingFontId, pageText.c_str());
  }

  // 2-bit style state (bit0=heading, bit1=bold -- see
  // CjkVerticalLayout::STYLE_BIT_HEADING/STYLE_BIT_BOLD) active right now,
  // threaded through the column loop below starting from this page's known
  // opening state (a heading and/or bold run spanning a page boundary
  // carries over correctly without rescanning from the chapter start).
  uint8_t currentStyle = static_cast<size_t>(currentPage) < pageStartStyle.size() ? pageStartStyle[currentPage] : 0;

  int columnIndex = 0;
  size_t columnStart = pageStart;

  while (columnStart < pageEnd && columnIndex < metrics.columnsPerPage) {
    const size_t columnEnd =
        std::min(pageEnd, CjkVerticalLayout::nextColumnEnd(chapterText, columnStart, metrics,
                                                           CJK_READER_SETTINGS.kinsokuEnabled));

    const int columnX = viewportRightX - glyphAdvancePx - columnIndex * columnWidthPx;

    // One column's worth of text (bounded by rowsPerColumn, always small) --
    // chapterText is file-backed, not an in-RAM buffer, so read this
    // column's range out once for the walk below rather than assuming a
    // data() pointer. See CjkChapterText.h for why chapterText is on disk.
    const std::string columnText = CjkVerticalLayout::extractRange(chapterText, columnStart, columnEnd);
    const auto* base = reinterpret_cast<const unsigned char*>(columnText.data());
    const unsigned char* cursor = base;
    const unsigned char* end = base + columnText.size();
    int row = 0;
    std::string glyphUtf8;
    while (cursor < end) {
      const unsigned char* before = cursor;
      const uint32_t cp = utf8NextCodepoint(&cursor);
      // See CjkVerticalLayout::nextColumnEnd: a NUL byte makes
      // utf8NextCodepoint() return without advancing, which would spin this
      // loop forever. Should be unreachable (CjkChapterText strips control
      // bytes and nextColumnEnd never hands back a range containing one),
      // but bail out rather than hang if it ever is.
      if (cursor == before) break;

      // Zero-width text-level marker (see CjkVerticalLayout.h) -- update
      // state and move on without drawing or advancing to the next row.
      if (CjkVerticalLayout::isStyleSentinel(cp)) {
        currentStyle = CjkVerticalLayout::styleFromSentinel(cp);
        continue;
      }

      const bool isHeadingActive = (currentStyle & CjkVerticalLayout::STYLE_BIT_HEADING) != 0;
      const bool isBoldActive = (currentStyle & CjkVerticalLayout::STYLE_BIT_BOLD) != 0;
      const EpdFontFamily::Style drawStyle = isBoldActive ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

      // headingFontId may be 0 (family has no larger size installed -- see
      // onEnter()'s comment), in which case headings just render at the
      // normal size rather than failing.
      const int drawFontId = (isHeadingActive && headingFontId != 0) ? headingFontId : fontId;

      glyphUtf8.clear();
      utf8AppendCodepoint(cp, glyphUtf8);
      const int y = viewportTopY + row * glyphAdvancePx;

      // Prefer a real Unicode Vertical Forms glyph over rotating the
      // horizontal one, when the loaded font actually has it (only true if
      // it was converted with that codepoint range included -- see
      // CjkKinsoku::verticalFormFor's own comment). Drawn exactly like any
      // other upright character: no rotation, no offset, since these glyphs
      // are purpose-built to sit correctly in their own advance box for
      // vertical use. Confirmed against the font's real bitmap data (not
      // just assumed) to look correct for every mapped pair -- see the
      // rotation-vs-real-glyph specimen this was validated against.
      const uint32_t verticalCp = CjkKinsoku::verticalFormFor(cp);
      const auto fontIt = verticalCp != 0 ? renderer.getFontMap().find(drawFontId) : renderer.getFontMap().end();
      const bool hasVerticalForm = verticalCp != 0 && fontIt != renderer.getFontMap().end() &&
                                   fontIt->second.hasCodepoint(verticalCp, drawStyle);

      if (hasVerticalForm) {
        std::string verticalUtf8;
        utf8AppendCodepoint(verticalCp, verticalUtf8);
        renderer.drawText(drawFontId, columnX, y, verticalUtf8.c_str(), true, drawStyle);
      } else if (CjkKinsoku::isRotatedPunctuation(cp)) {
        // Cell is [columnX, columnX+glyphAdvancePx) x [y, y+glyphAdvancePx)
        // -- the same fixed step every other glyph in this column uses. A
        // heading-sized rotated glyph is drawn at this same fixed cell too
        // (the deliberate "same grid step, some overflow" trade-off -- see
        // headingFontId's own comment).
        renderer.drawGlyphRotated90CCW(drawFontId, cp, columnX, y, glyphAdvancePx, true, drawStyle,
                                       CjkKinsoku::isCenteredRotation(cp));
      } else {
        int8_t dx = 0;
        int8_t dy = 0;
        CjkKinsoku::getVerticalPunctuationOffset(cp, static_cast<uint16_t>(glyphAdvancePx),
                                                 static_cast<uint16_t>(glyphAdvancePx), dx, dy);
        renderer.drawText(drawFontId, columnX + dx, y + dy, glyphUtf8.c_str(), true, drawStyle);
      }

      row++;
    }

    columnStart = columnEnd;
    columnIndex++;
  }
}

void CjkVerticalReaderActivity::renderStatusBar() const {
  // GUI.drawStatusBar, not a hand-rolled page-number label: the same
  // shared (non-virtual, theme-independent) BaseTheme method
  // EpubReaderActivity uses, so this reader's status bar matches the
  // standard reader's layout/content (progress %, progress bar, battery --
  // whatever SETTINGS.statusBarSpec() has enabled) instead of a bespoke,
  // sparser one. Its own Y position already comes from
  // UITheme::getStatusBarHeight(), the same metric viewportBottomY already
  // reserves room for in onEnter() -- see viewportBottomY's own margin
  // calc -- so the two agree without needing a separate size to track.
  const int totalPages = std::max<int>(1, static_cast<int>(pageIndex.size()) - 1);
  const int currentPageNum = currentPage + 1;
  const float chapterProgress =
      totalPages > 0 ? static_cast<float>(currentPageNum) / static_cast<float>(totalPages) : 0.0f;
  const float bookProgress = epub ? epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f : 0.0f;
  GUI.drawStatusBar(renderer, bookProgress, currentPageNum, totalPages, epub ? epub->getTitle() : "");
}
