#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <optional>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "SdCardFontSystem.h"
#include "cjk/CjkReaderSettings.h"
#include "cjk/CjkVerticalReaderActivity.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

int ReaderActivity::initialRefreshCountdown() const {
  if (!allowFastInitialRefresh) return 0;

  const int refreshFrequency = SETTINGS.getRefreshFrequency();
  return refreshFrequency > 1 ? refreshFrequency : 2;
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds. Show the
  // indexing popup so it isn't a silent wait on the home screen. The cachePath/hash is known at
  // construction, so this check is valid before load(); a cached open loads in a blink -> no popup.
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    // The popup replaces the restored Quick Resume frame, so the reader must clean it.
    allowFastInitialRefresh = false;
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }
  bool loaded;
  {
    // Lend the framebuffer's 48 KB to the container parse (expat + spine/TOC
    // build). The popup just displayed stays on the panel; whichever reader
    // activity follows redraws the full screen anyway.
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = epub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (loaded) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::openEpubWithChoice(std::unique_ptr<Epub> epub, const Epub::ReaderChoice choice) {
  if (choice == Epub::ReaderChoice::Cjk) {
    activityManager.replaceActivity(std::make_unique<CjkVerticalReaderActivity>(renderer, mappedInput,
                                                                                std::move(epub),
                                                                                initialRefreshCountdown()));
    return;
  }
  activityManager.replaceActivity(
      std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub), initialRefreshCountdown()));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;

  // The CJK reader is only a real choice when explicitly enabled AND a font family is
  // configured (there's no built-in CJK font to fall back on). Any other state -- disabled,
  // or enabled with no font picked yet -- means there's nothing to ask: always Default,
  // exactly as before this per-book chooser existed.
  if (!(CJK_READER_SETTINGS.enabled && !CJK_READER_SETTINGS.fontFamilyName.empty())) {
    openEpubWithChoice(std::move(epub), Epub::ReaderChoice::Default);
    return;
  }

  const Epub::ReaderChoice saved = epub->getSavedReaderChoice();
  if (saved != Epub::ReaderChoice::Unset) {
    openEpubWithChoice(std::move(epub), saved);
    return;
  }

  // First time this particular book has been opened with the CJK reader available: ask once,
  // then remember the answer in the book's own cache dir so it never needs asking again.
  pendingEpub = std::move(epub);
  static constexpr StrId options[] = {StrId::STR_READER_DEFAULT, StrId::STR_READER_CJK_VERTICAL};
  readerChoicePopup.show(StrId::STR_CHOOSE_READER_TITLE, options, 2, 0, [this](const int idx) {
    auto chosenEpub = std::move(pendingEpub);
    if (!chosenEpub) return;
    const auto choice = idx == 1 ? Epub::ReaderChoice::Cjk : Epub::ReaderChoice::Default;
    chosenEpub->saveReaderChoice(choice);
    openEpubWithChoice(std::move(chosenEpub), choice);
  });
  requestUpdate();
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(
      std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc), initialRefreshCountdown()));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(
      std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt), initialRefreshCountdown()));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  sdFontSystem.ensureLoaded(renderer);

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }

void ReaderActivity::loop() {
  if (readerChoicePopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    // handleInput() returns true both when an option was picked (onSelect already ran and
    // consumed pendingEpub) and when the popup was merely dismissed -- physical Back, or a tap
    // outside the dialog -- with no callback fired. In the latter case pendingEpub is still
    // set: without bailing out here, the reader would sit on a frozen screen forever, since
    // loop() has nothing else to check once the popup goes inactive.
    if (!readerChoicePopup.isActive() && pendingEpub) {
      pendingEpub.reset();
      onGoBack();
    }
    return;
  }
}

void ReaderActivity::render(RenderLock&&) {
  if (!readerChoicePopup.isActive()) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 pendingEpub ? pendingEpub->getTitle().c_str() : "");

  if (readerChoicePopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}
