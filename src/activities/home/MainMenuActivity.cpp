#include "MainMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/icons/read80.h"
#include "components/icons/tasks80.h"
#include "fontIds.h"

namespace {
// The status bar's own vertical extent, as QuickPickActivity lays it out (see
// its HEADER_CONTENT_HEIGHT / HEADER_HIGHLIGHT_HEIGHT): the theme's header rect
// is sized for a full title band this screen does not draw, so the layout below
// uses the height of just the clock/battery row.
constexpr int STATUS_BAR_HEIGHT = 36;

// The tile artwork: the source bitmaps are 80x80, drawn at up to this size (70% of
// the 160px they were first drawn at), and never smaller than MIN_ICON_SIZE even when
// the band above takes a lot of room.
constexpr int SOURCE_ICON_SIZE = 80;
constexpr int MAX_ICON_SIZE = 112;
constexpr int MIN_ICON_SIZE = 64;
// The least space left above, between and below the two options before the icons
// shrink to make more.
constexpr int MIN_GAP = 8;
// The label sits this far under its icon, and the selection outline stands this
// far (times 4 on the sides, as the old home grid did) clear of the pair.
constexpr int LABEL_GAP = 12;
constexpr int SELECTION_PADDING = 8;
constexpr int SELECTION_LINE_WIDTH = 2;
constexpr int SELECTION_CORNER_RADIUS = 6;

// Draws the 80x80 1-bpp icon `bitmap` (MSB first, bit 0 = ink, the format
// GfxRenderer::drawIcon() takes) at `size` x `size` with its top-left at (x, y),
// using the same orientation mapping as drawIcon(). Each output pixel samples the
// source bilinearly (8-bit fixed point, no floats on this core) and is inked when
// at least half of the sample is ink, so the edges of the line art stay smooth
// instead of stepping at the scale factor.
void drawScaledIcon(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const int size) {
  constexpr int ROW_BYTES = (SOURCE_ICON_SIZE + 7) / 8;
  const auto inkAt = [bitmap](const int row, const int col) -> int {
    if (row < 0 || col < 0 || row >= SOURCE_ICON_SIZE || col >= SOURCE_ICON_SIZE) return 0;
    const uint8_t byte = bitmap[row * ROW_BYTES + (col >> 3)];
    return ((byte >> (7 - (col & 7))) & 1) == 0 ? 1 : 0;
  };

  // Source position of each output pixel's centre, in 1/256ths of a source pixel.
  const auto sourcePos = [size](const int out) -> int { return ((2 * out + 1) * SOURCE_ICON_SIZE * 128) / size - 128; };

  for (int row = 0; row < size; row++) {
    const int fy = sourcePos(row);
    const int y0 = fy >> 8;  // arithmetic shift: floors for the (small) negative edge
    const int wy1 = fy & 255;
    const int wy0 = 256 - wy1;
    for (int col = 0; col < size; col++) {
      const int fx = sourcePos(col);
      const int x0 = fx >> 8;
      const int wx1 = fx & 255;
      const int wx0 = 256 - wx1;
      const int weight = wy0 * (wx0 * inkAt(y0, x0) + wx1 * inkAt(y0, x0 + 1)) +
                         wy1 * (wx0 * inkAt(y0 + 1, x0) + wx1 * inkAt(y0 + 1, x0 + 1));
      // The four weights sum to 256 * 256 = 65536, reached when every neighbour is ink.
      if (weight >= 32768) renderer.drawPixel(x + (size - 1 - row), y + col, true);
    }
  }
}
}  // namespace

void MainMenuActivity::onEnter() {
  Activity::onEnter();
  // Back on the tile of the area just left: the Planner screen's Home puts focus on
  // the Planner, the Reader's on the Reader (see setReturnArea()).
  focus = returnArea == HomeArea::Planner ? Stop::Planner : Stop::Read;
  bandStop = StatusBand::Stop::StatusBar;
  band.poll();
  settingsPressSeen = false;
  swallowConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Right2);
  requestUpdate();
}

MainMenuActivity::TileLayout MainMenuActivity::tileLayout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // From just below the band as it was last drawn (the status bar, plus whatever
  // calendar rows and alert are showing) down to the front-button labels.
  const int top = band.bottom(renderer);
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.buttonHintsGap;
  const int available = bottom - top;

  // Everything in an option's box except its icon: the label under it and the padding
  // around both.
  const int chrome = LABEL_GAP + renderer.getLineHeight(UI_12_FONT_ID) + 2 * SELECTION_PADDING;
  // As large as the artwork goes while two boxes and three gaps of at least MIN_GAP
  // still fit.
  const int fitIcon = (available - 3 * MIN_GAP) / 2 - chrome;
  const int iconSize = std::max(MIN_ICON_SIZE, std::min(MAX_ICON_SIZE, fitIcon));
  const int boxHeight = iconSize + chrome;

  // Three equal gaps: above the first box, between the two, below the second.
  const int gap = std::max(0, (available - 2 * boxHeight) / 3);
  const int firstBoxTop = top + gap;
  return TileLayout{iconSize, boxHeight, gap, firstBoxTop, firstBoxTop + boxHeight + gap};
}

void MainMenuActivity::activateFocused() {
  switch (focus) {
    case Stop::Band:
      if (bandStop == StatusBand::Stop::StatusBar) {
        // Returns here once the sync is done; goHome() is the one mechanism every
        // other screen's own Sync All return uses too.
        activityManager.goToSyncAll([] { activityManager.goHome(); });
      } else {
        // Dismisses the alert (the next-newest takes its place); nothing to do on
        // the calendar rows.
        band.handleRight2(bandStop);
        bandStop = band.clamp(bandStop);
        requestUpdate();
      }
      break;
    case Stop::Read:
      activityManager.goToReadMenu();
      break;
    case Stop::Planner:
      activityManager.goToCompanion();
      break;
  }
}

void MainMenuActivity::loop() {
  // An alert arriving or being dismissed elsewhere, or a calendar event ending, changes
  // what the band shows: repaint, and keep focus on a stop that still exists.
  if (band.poll()) requestUpdate();
  if (focus == Stop::Band) bandStop = band.clamp(bandStop);

  // One continuous loop: the band's stops, Reader, Planner, and back around.
  buttonNavigator.onNext([this] {
    if (focus == Stop::Band) {
      if (!band.step(bandStop, /*forward=*/true)) focus = Stop::Read;
    } else if (focus == Stop::Read) {
      focus = Stop::Planner;
    } else {
      focus = Stop::Band;
      bandStop = band.first();
    }
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    if (focus == Stop::Band) {
      if (!band.step(bandStop, /*forward=*/false)) focus = Stop::Planner;
    } else if (focus == Stop::Read) {
      focus = Stop::Band;
      bandStop = band.last();
    } else {
      focus = Stop::Read;
    }
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Right1)) settingsPressSeen = true;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right2)) swallowConfirmRelease = false;

  // Right1 is Settings from every stop except the calendar rows and the alert (it
  // opens those screens) -- there is nowhere to go back to from Home. settingsPressSeen
  // guards against the stale release of the press that closed the previous screen.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right1) && settingsPressSeen) {
    if (focus == Stop::Band && band.handleRight1(bandStop)) return;
    activityManager.goToSettings();
    return;
  }

  // A tap on an option selects and opens it. Each option's tap area is its box plus
  // half the gap on either side, so the two meet in the middle of the gap between them.
  const TileLayout layout = tileLayout();
  const int width = renderer.getScreenWidth();
  const int halfGap = layout.gap / 2;
  if (mappedInput.wasTapInRect(0, layout.firstBoxTop - halfGap, width, layout.boxHeight + 2 * halfGap)) {
    focus = Stop::Read;
    activateFocused();
    return;
  }
  if (mappedInput.wasTapInRect(0, layout.secondBoxTop - halfGap, width, layout.boxHeight + 2 * halfGap)) {
    focus = Stop::Planner;
    activateFocused();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right2)) {
    if (swallowConfirmRelease) {
      swallowConfirmRelease = false;
      return;
    }
    activateFocused();
  }
}

void MainMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  // The top of the screen is the shared status band: the theme's clock/date/battery
  // row, today's calendar events, the newest pending alert (see StatusBand). This
  // screen has no title of its own, so it is drawn untitled.
  if (focus == Stop::Band) bandStop = band.clamp(bandStop);
  band.drawUntitled(renderer, metrics.topPadding, STATUS_BAR_HEIGHT,
                    focus == Stop::Band ? bandStop : StatusBand::Stop::None);

  const TileLayout layout = tileLayout();
  const int labelHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int iconSize = layout.iconSize;

  struct Tile {
    Stop stop;
    const uint8_t* icon;
    const char* label;
  };
  const Tile tiles[] = {
      {Stop::Read, Read80Icon, tr(STR_MENU_READER)},
      {Stop::Planner, Tasks80Icon, tr(STR_MENU_PLANNER)},
  };

  for (int i = 0; i < 2; i++) {
    const bool selected = focus == tiles[i].stop;
    const auto style = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string label = renderer.truncatedText(UI_12_FONT_ID, tiles[i].label, pageWidth - 32, style);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), style);

    // The icon and its label, inside the option's box.
    const int contentHeight = iconSize + LABEL_GAP + labelHeight;
    const int boxTop = i == 0 ? layout.firstBoxTop : layout.secondBoxTop;
    const int contentTop = boxTop + SELECTION_PADDING;

    if (selected) {
      // An outline, not a fill: the artwork is line work, and inverting a tile
      // this size is a lot of ink to move on every selection change.
      const int boxWidth = std::max(iconSize, labelWidth) + SELECTION_PADDING * 4;
      renderer.drawRoundedRect((pageWidth - boxWidth) / 2, boxTop, boxWidth, layout.boxHeight, SELECTION_LINE_WIDTH,
                               SELECTION_CORNER_RADIUS, true);
    }

    drawScaledIcon(renderer, tiles[i].icon, (pageWidth - iconSize) / 2, contentTop, iconSize);
    renderer.drawText(UI_12_FONT_ID, (pageWidth - labelWidth) / 2, contentTop + iconSize + LABEL_GAP, label.c_str(),
                      true, style);
  }

  // Right1 is Settings (the calendar rows and the alert open their own screens);
  // Right2 is Sync All on the status bar, Dismiss on the alert, and Select on a tile.
  const bool onBandAction = focus == Stop::Band && bandStop != StatusBand::Stop::StatusBar;
  const auto labels = mappedInput.mapLabels(onBandAction ? band.right1Label(bandStop) : tr(STR_SETTINGS_TITLE),
                                            focus == Stop::Band ? band.right2Label(bandStop) : tr(STR_SELECT),
                                            tr(STR_DIR_PREV), tr(STR_DIR_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
