#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
}  // namespace

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    return;
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(fileNameBuffer.get()) + "/");
    } else {
      std::string_view filename{fileNameBuffer.get()};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (mode == Mode::PickImage) {
        // Image picker: only show .bmp files -- the only format the bitmap
        // loader used everywhere a picked image is later shown understands.
        if (FsHelpers::hasBmpExtension(filename)) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  selectorIndex = 0;
  headerFocused = false;
  bandStop = StatusBand::Stop::StatusBar;
  band.setEnabled(mode == Mode::Books);
  band.poll();

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Right2);

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Right1);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  fileNameBuffer.reset();
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

void FileBrowserActivity::loop() {
  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && mappedInput.isPressed(MappedInputManager::Button::Right1) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/" && !lockLongPressBack) {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Right1)) {
    lockLongPressBack = false;
    return;
  }

  // An alert arriving or being dismissed elsewhere, or a calendar event ending, changes
  // what the band shows: repaint, and keep focus on a stop that still exists.
  if (band.poll()) requestUpdate();
  if (headerFocused) bandStop = band.clamp(bandStop);

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = band.bottom(renderer) + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.buttonHintsGap - pathReserved;
  // From the room the list really has (the band can have pushed it down), not the
  // theme's assumption of a plain header.
  const int pageItems = GUI.getListPageItems(contentHeight, false);

  auto activateSelected = [this] {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    // Firmware/image picker: select file -> return path; navigate into directories normally.
    if ((mode == Mode::PickFirmware || mode == Mode::PickImage) && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult res{FilePathResult{cleanBasePath + entry}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (mode == Mode::Books && mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: DELETE FILE OR DIRECTORY ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string fullPath = cleanBasePath + entry;

      auto handler = [this, fullPath](const ActivityResult& res) {
        // The confirmation popup acts on button press; if that button is still
        // held when we resume, swallow its release so it doesn't also act here
        // (Back would go up a directory, Confirm would open the selection).
        lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Right1);
        lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Right2);
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          if (removeDirFile(fullPath)) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            loadFiles();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= files.size()) {
              // Move selection to the new "last" item
              selectorIndex = files.size() - 1;
            }

            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("FileBrowser", "Delete cancelled by user");
        }
      };

      std::string heading = tr(STR_DELETE) + std::string("? ");

      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  };

  int touchSel = static_cast<int>(selectorIndex);
  const auto listTouch = handleListTouch(touchSel, static_cast<int>(files.size()), contentTop, contentHeight, false);
  if (listTouch != ListTouchResult::None) {
    selectorIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) activateSelected();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right2)) {
    if (mode == Mode::Books && headerFocused) {
      if (bandStop != StatusBand::Stop::StatusBar) {
        // Dismisses the alert (the next-newest takes its place); nothing to do on
        // the calendar rows.
        band.handleRight2(bandStop);
        bandStop = band.clamp(bandStop);
        requestUpdate();
        return;
      }
      // Captured by value: this screen is destroyed the instant
      // goToSyncAll()'s own replaceActivity() call runs.
      activityManager.goToSyncAll(
          [path = basepath, backToRead = returnToReadMenu] { activityManager.goToFileBrowser(path, backToRead); });
      return;
    }
    activateSelected();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right1)) {
    // Short press: go up one directory, or go home if at root -- or, with the
    // status bar focused, always home (labelled "« Home", see the hints below).
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      // On the calendar rows or the alert, Right1 opens that screen.
      if (mode == Mode::Books && headerFocused && band.handleRight1(bandStop)) return;
      if (mode == Mode::Books && headerFocused) {
        onGoHome();
      } else if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else if (mode == Mode::PickFirmware || mode == Mode::PickImage) {
        // Firmware/image picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else if (returnToReadMenu) {
        activityManager.goToReadMenu();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    // A swipe is a page-jump, not a single step, so it always leaves the
    // header stop (if it was focused) -- same reasoning as
    // PlannerScreenActivity's own swipe handling.
    headerFocused = false;
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    headerFocused = false;
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, listSize] {
    if (headerFocused) {
      // Down through the band's stops, then into the list.
      if (!band.step(bandStop, /*forward=*/true)) {
        headerFocused = false;
        selectorIndex = 0;
      }
    } else if (mode == Mode::Books && static_cast<int>(selectorIndex) == listSize - 1) {
      headerFocused = true;
      bandStop = band.first();
    } else {
      selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    }
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    if (headerFocused) {
      // Up through the band's stops, then wrap to the end of the list.
      if (!band.step(bandStop, /*forward=*/false)) {
        headerFocused = false;
        selectorIndex = listSize > 0 ? static_cast<size_t>(listSize - 1) : 0;
      }
    } else if (mode == Mode::Books && selectorIndex == 0) {
      headerFocused = true;
      bandStop = band.last();
    } else {
      selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    }
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    headerFocused = false;
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    headerFocused = false;
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(const std::string& filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName = (mode == Mode::PickFirmware) ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
                           : (mode == Mode::PickImage)  ? std::string(tr(STR_SELECT_IMAGE_FILE))
                                                        : ((basepath == "/") ? std::string(tr(STR_SD_CARD))
                                                                             : basepath.substr(basepath.rfind('/') + 1));
  if (headerFocused) bandStop = band.clamp(bandStop);
  const int headerBottom = band.drawTitled(renderer, folderName.c_str(), nullptr,
                                           (mode == Mode::Books && headerFocused) ? bandStop : StatusBand::Stop::None);

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = headerBottom + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.buttonHintsGap - pathReserved;
  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(),
        headerFocused ? -1 : static_cast<int>(selectorIndex), [this](int index) { return getFileName(files[index]); },
        nullptr, [this](int index) { return UITheme::getFileIcon(files[index]); },
        [this](int index) { return getFileExtension(files[index]); }, false);
  }

  // Full path display
  {
    const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.buttonHintsGap - pathLineHeight;
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  const bool cancelsToCallerAtRoot = mode == Mode::PickFirmware || mode == Mode::PickImage;
  const char* backLabel = (mode == Mode::Books && headerFocused) ? band.right1Label(bandStop)
                          : (basepath == "/")
                              ? ((cancelsToCallerAtRoot || returnToReadMenu) ? tr(STR_BACK) : tr(STR_HOME))
                              : tr(STR_BACK);
  // In PickFirmware/PickImage mode, Confirm on a file returns its path to the caller (not
  // "open"); show STR_SELECT instead. Directories in the same picker still descend, so keep
  // STR_OPEN there.
  const bool selectingReturnableFile = cancelsToCallerAtRoot && !files.empty() && files[selectorIndex].back() != '/';
  const char* confirmLabel = (mode == Mode::Books && headerFocused)
                                 ? band.right2Label(bandStop)
                                 : (files.empty() ? "" : (selectingReturnableFile ? tr(STR_SELECT) : tr(STR_OPEN)));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
