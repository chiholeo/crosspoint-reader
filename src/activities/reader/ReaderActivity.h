#pragma once
#include <memory>

#include "Epub.h"
#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"
#include "components/OptionPopup.h"

class Xtc;
class Txt;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  bool allowFastInitialRefresh;
  // Non-static (unlike the other loaders): draws the first-open indexing popup, which needs the renderer.
  std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isBmpFile(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();
  int initialRefreshCountdown() const;

  // Per-book override of which reader engine to use, letting a mixed English/CJK library use
  // the right reader per book instead of one global CJK-reader-enabled setting for every EPUB.
  // The choice itself is persisted via Epub::getSavedReaderChoice()/saveReaderChoice(), in the
  // book's own cache directory -- survives independently of RecentBooksStore's recency-limited
  // list, so it still applies after a book falls out of "recent" and is reopened via the file
  // browser much later, and only needs asking once per book.
  //
  // pendingEpub is held across loop()/render() calls while readerChoicePopup is up deciding
  // which reader to use -- onGoToEpubReader() can't finish the dispatch synchronously in that
  // case the way it does when there's nothing to ask (CJK reader not viable, or a choice
  // already saved).
  std::unique_ptr<Epub> pendingEpub;
  OptionPopup readerChoicePopup;
  void openEpubWithChoice(std::unique_ptr<Epub> epub, Epub::ReaderChoice choice);

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          bool allowFastInitialRefresh)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        allowFastInitialRefresh(allowFastInitialRefresh) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
