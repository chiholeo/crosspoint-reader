#include "CjkVerticalReaderActivity.h"

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
}  // namespace

void CjkVerticalReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    finish();
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  const auto path = epub->getPath();
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(path, epub->getTitle(), epub->getAuthor(), epub->getCoverBmpPath());

  if (CJK_READER_SETTINGS.fontFamilyName.empty()) {
    LOG_ERR("CJKR", "CJK reader enabled with no font family configured");
    loadFailed = true;
    loadFailMessage = tr(STR_CJK_NO_FONT_SET);
    requestUpdate();
    return;
  }

  fontRegistry.discover();
  const auto* family = fontRegistry.findFamily(CJK_READER_SETTINGS.fontFamilyName);
  if (!family) {
    LOG_ERR("CJKR", "CJK font family '%s' not found on SD card (registry has %d families)",
            CJK_READER_SETTINGS.fontFamilyName.c_str(), fontRegistry.getFamilyCount());
    loadFailed = true;
    loadFailMessage = tr(STR_CJK_FONT_NOT_FOUND);
    requestUpdate();
    return;
  }
  // ReaderActivity::onEnter() unconditionally loads the standard reader's
  // SD font (sdFontSystem.ensureLoaded()) before dispatching here. Font IDs
  // are content-hash based and global to the renderer, so if this is the
  // same family+size the standard reader is already using, loading it again
  // under a second SdCardFontManager fails as an ID collision rather than
  // succeeding. Free that slot first -- restored in onExit().
  sdFontSystem.unload(renderer);
  standardFontUnloaded = true;

  if (!fontManager.loadFamily(*family, renderer, CJK_READER_SETTINGS.fontPointSize)) {
    LOG_ERR("CJKR", "Failed to load CJK font family '%s' at %u pt", CJK_READER_SETTINGS.fontFamilyName.c_str(),
            CJK_READER_SETTINGS.fontPointSize);
    loadFailed = true;
    loadFailMessage = tr(STR_CJK_FONT_LOAD_ERROR);
    requestUpdate();
    return;
  }
  fontId = fontManager.getFontId(CJK_READER_SETTINGS.fontFamilyName);

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

  const int cellSize = renderer.getLineHeight(fontId);
  glyphAdvancePx = std::max(cellSize, 1);
  columnWidthPx = glyphAdvancePx + columnSpacingPx(CJK_READER_SETTINGS.columnSpacing);

  metrics.rowsPerColumn = std::max(1, (viewportBottomY - viewportTopY) / glyphAdvancePx);
  metrics.columnsPerPage = std::max(1, (viewportRightX - viewportLeftX) / columnWidthPx);

  Progress resume;
  const bool hasResume = loadProgress(resume);
  const int startSpine = hasResume ? resume.spineIndex : 0;
  if (loadChapter(startSpine)) {
    if (hasResume) seekToByteOffset(resume.byteOffset);
  } else if (startSpine != 0 && loadChapter(0)) {
    // Saved chapter no longer resolves (different/edited book sharing this
    // cache dir) -- start over from the beginning rather than fail outright.
  } else {
    loadFailed = true;
  }

  initialized = true;
  requestUpdate();
}

void CjkVerticalReaderActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  fontManager.unloadAll(renderer);
  fontId = 0;
  // Restore the standard reader's SD font if we unloaded it (see onEnter):
  // other screens (file browser, home) use it for CJK book-title fallback
  // rendering, and it would otherwise stay unloaded until the next book open.
  if (standardFontUnloaded) sdFontSystem.ensureLoaded(renderer);
  chapterText.clear();
  pageIndex.clear();
  // Clears the crash-loop guard main.cpp sets on boot-resume: reaching a
  // clean onExit() means this session didn't crash mid-read. Every other
  // reader activity clears this the same way.
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  epub.reset();
}

bool CjkVerticalReaderActivity::loadChapter(const int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) return false;

  const auto spineItem = epub->getSpineItem(spineIndex);
  size_t size = 0;
  uint8_t* bytes = epub->readItemContentsToBytes(spineItem.href, &size, true);
  if (!bytes) {
    LOG_ERR("CJKR", "Failed to read chapter %d (%s)", spineIndex, spineItem.href.c_str());
    return false;
  }
  chapterText = CjkChapterText::extractPlainText(reinterpret_cast<const char*>(bytes), size);
  free(bytes);

  currentSpineIndex = spineIndex;
  currentPage = 0;

  // Prewarm the whole chapter's SD glyphs in one batch rather than letting
  // per-page rendering trigger the expensive per-glyph SD-miss path
  // repeatedly (see add_CJKreader.md).
  if (fontId != 0) {
    renderer.ensureSdCardFontReady(fontId, chapterText.c_str());
  }

  rebuildPageIndexForCurrentChapter();
  return true;
}

void CjkVerticalReaderActivity::rebuildPageIndexForCurrentChapter() {
  pageIndex = CjkVerticalLayout::buildPageIndex(chapterText, metrics, CJK_READER_SETTINGS.kinsokuEnabled);
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

void CjkVerticalReaderActivity::openChapterSelection() {
  const int spineAtOpen = currentSpineIndex;
  const std::string path = epub->getPath();
  startActivityForResult(
      std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineAtOpen),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto& chapterResult = std::get<ChapterResult>(result.data);
        RenderLock lock(*this);
        if (chapterResult.spineIndex != currentSpineIndex) {
          loadChapter(chapterResult.spineIndex);
        }
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

  if (prevTriggered) {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    } else if (currentSpineIndex > 0 && loadChapter(currentSpineIndex - 1)) {
      const int newTotalPages = std::max<int>(1, static_cast<int>(pageIndex.size()) - 1);
      currentPage = newTotalPages - 1;
      requestUpdate();
    }
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      if (loadChapter(currentSpineIndex + 1)) requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void CjkVerticalReaderActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (loadFailed || !initialized || pageIndex.size() < 2) {
    const std::string message = loadFailed && !loadFailMessage.empty() ? loadFailMessage : tr(STR_CJK_LOAD_FAILED);
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, message.c_str());
    renderer.displayBuffer();
    return;
  }

  renderPage();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  saveProgress();
}

void CjkVerticalReaderActivity::renderPage() const {
  const size_t pageStart = pageIndex[currentPage];
  const size_t pageEnd = pageIndex[currentPage + 1];

  int columnIndex = 0;
  size_t columnStart = pageStart;

  while (columnStart < pageEnd && columnIndex < metrics.columnsPerPage) {
    const size_t columnEnd =
        std::min(pageEnd, CjkVerticalLayout::nextColumnEnd(chapterText, columnStart, metrics,
                                                           CJK_READER_SETTINGS.kinsokuEnabled));

    const int columnX = viewportRightX - glyphAdvancePx - columnIndex * columnWidthPx;

    const auto* base = reinterpret_cast<const unsigned char*>(chapterText.data());
    const unsigned char* cursor = base + columnStart;
    const unsigned char* end = base + columnEnd;
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

      glyphUtf8.clear();
      utf8AppendCodepoint(cp, glyphUtf8);
      const int y = viewportTopY + row * glyphAdvancePx;

      if (CjkKinsoku::isRotatedPunctuation(cp)) {
        // drawTextRotated90CW renders upward from the given y (y is the
        // bottom end of the run, unlike drawText's top-anchored y) -- see
        // FreeInkUIGfxRenderer.h's usage comment. Anchor at this cell's
        // bottom edge so the rotated glyph fills the same [y, y+glyphAdvancePx)
        // span an unrotated glyph in this row would.
        renderer.drawTextRotated90CW(fontId, columnX, y + glyphAdvancePx, glyphUtf8.c_str());
      } else {
        int8_t dx = 0;
        int8_t dy = 0;
        CjkKinsoku::getVerticalPunctuationOffset(cp, static_cast<uint16_t>(glyphAdvancePx),
                                                 static_cast<uint16_t>(glyphAdvancePx), dx, dy);
        renderer.drawText(fontId, columnX + dx, y + dy, glyphUtf8.c_str());
      }

      row++;
    }

    columnStart = columnEnd;
    columnIndex++;
  }
}

void CjkVerticalReaderActivity::renderStatusBar() const {
  const int totalPages = std::max<int>(1, static_cast<int>(pageIndex.size()) - 1);
  const std::string pageLabel = std::to_string(currentPage + 1) + " / " + std::to_string(totalPages);
  const int y = viewportBottomY + (renderer.getScreenHeight() - viewportBottomY - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, y, pageLabel.c_str());
}
