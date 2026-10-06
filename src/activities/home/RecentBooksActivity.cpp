#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  headerFocused = false;
  bandStop = StatusBand::Stop::StatusBar;
  band.poll();
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  // An alert arriving or being dismissed elsewhere, or a calendar event ending, changes
  // what the band shows: repaint, and keep focus on a stop that still exists.
  if (band.poll()) requestUpdate();
  if (headerFocused) bandStop = band.clamp(bandStop);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = band.bottom(renderer) + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.buttonHintsGap;
  // From the room the list really has (the band can have pushed it down), not the
  // theme's assumption of a plain header.
  const int pageItems = GUI.getListPageItems(contentHeight, true);

  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Right2)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!headerFocused && !recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Right2) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right2)) {
    if (headerFocused) {
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
      activityManager.goToSyncAll([backToRead = returnToReadMenu] { activityManager.goToRecentBooks(backToRead); });
      return;
    }
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  int touchSel = static_cast<int>(selectorIndex);
  const auto listTouch =
      handleListTouch(touchSel, static_cast<int>(recentBooks.size()), contentTop, contentHeight, true);
  if (listTouch != ListTouchResult::None) {
    selectorIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) {
      LOG_DBG("RBA", "Tapped recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right1)) {
    // On the calendar rows or the alert, Right1 opens that screen.
    if (headerFocused && band.handleRight1(bandStop)) return;
    // With the status bar focused Right1 is always "« Home" (see render()), even
    // when this screen was opened from the Read menu.
    if (returnToReadMenu && !headerFocused) {
      activityManager.goToReadMenu();
    } else {
      onGoHome();
    }
  }

  int listSize = static_cast<int>(recentBooks.size());
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
    } else if (static_cast<int>(selectorIndex) == listSize - 1) {
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
    } else if (selectorIndex == 0) {
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

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= recentBooks.size()) {
        selectorIndex = recentBooks.size() - 1;
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (headerFocused) bandStop = band.clamp(bandStop);
  const int headerBottom =
      band.drawTitled(renderer, tr(STR_MENU_RECENT_BOOKS), nullptr, headerFocused ? bandStop : StatusBand::Stop::None);

  const int contentTop = headerBottom + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.buttonHintsGap;

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(),
        headerFocused ? -1 : static_cast<int>(selectorIndex), [this](int index) { return recentBooks[index].title; },
        [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
  }

  // Help text
  const auto labels = mappedInput.mapLabels(
      headerFocused ? band.right1Label(bandStop) : (returnToReadMenu ? tr(STR_BACK) : tr(STR_HOME)),
      headerFocused ? band.right2Label(bandStop) : tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
