#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "components/StatusBand.h"

class FileBrowserActivity final : public UiListActivity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // File actions
  bool removeDirFile(const std::string& fullPath);
  void showEntryActions();
  void startRename();
  void renameSelectedFile(const std::string& oldPath, const std::string& oldEntry, const std::string& newStem,
                          const std::string& extension);
  void deleteSelected();
  // Start folder (see CrossPointSettings::browseStartFolder): the highlighted folder's path without
  // a trailing slash, and whether it is the current start folder.
  std::string selectedFolderPath() const;
  bool isStartFolder(const std::string& path) const;
  void toggleStartFolder();

  Mode mode = Mode::Books;
  // When set, Back at the root returns to ReadMenuActivity instead of Home -- set only by
  // ActivityManager::goToFileBrowser() on behalf of ReadMenuActivity.
  bool returnToReadMenu = false;

  // An extra stop ahead of the first row (and after the last), the same idiom the other browsing
  // screens use -- Books mode only: a firmware/image picker is a narrow, single-purpose flow with no
  // reason to reach Sync All from it. The top of the screen -- status bar, today's calendar events,
  // the newest pending alert (see StatusBand) -- takes the place of the plain header there; while
  // headerFocused, bandStop says which of them has focus.
  bool headerFocused = false;
  StatusBand band;
  StatusBand::Stop bandStop = StatusBand::Stop::StatusBar;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;
  OptionPopup optionPopup;

  // Pull-based rows: the SDK list resolves each drawn row on demand through
  // provideRow() (fui::ListProps::rowProvider), so the only per-file
  // residency is `files` itself — no full-length rowNames/rowExtensions/
  // rowItems arrays (a 1000-file folder used to pin ~100KB of vectors plus a
  // heap copy of every display name, which aborted under -fno-exceptions
  // when the contiguous blocks no longer fit). The label/value strings for
  // the row being laid out live in these scratch buffers; the provider
  // contract only needs them valid until the next provideRow() call.
  static constexpr size_t ROW_NAME_BUF_SIZE = 512;  // NAME_BUFFER_SIZE + "[]" + terminator slack
  char rowNameBuf[ROW_NAME_BUF_SIZE]{};
  char rowExtBuf[16]{};
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  // CJK fallback glyphs are prewarmed for a bounded window of rows around the
  // viewport (one SD pass per list page, like the reader TOC) instead of the
  // whole folder. -1 = nothing prewarmed; reset by loadFiles().
  static constexpr int PREWARM_WINDOW = 24;
  int prewarmedStart = -1;
  void prewarmRowGlyphs(int start);

  int listCount() const override { return static_cast<int>(files.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Long-press BACK goes to root; short Back goes up a directory (home/cancel at
  // root), and Confirm activates on release while a hold opens file actions.
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  // Header shows the current folder name (battery indicator via GUI.drawHeader);
  // footer labels depend on path depth and picker mode.
  void drawChrome() override;
  void drawFooter() override;
  void activateSelected();

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books, bool returnToReadMenu = false);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;
};
