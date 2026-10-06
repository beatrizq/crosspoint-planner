#pragma once

#include "activities/Activity.h"
#include "components/StatusBand.h"
#include "util/ButtonNavigator.h"

/**
 * The app's main screen: the status bar (the theme's clock/date/battery row) and
 * two large tiles stacked vertically -- Reader and Planner. Reader opens the Read menu;
 * Planner opens the Companion screen (QuickPickActivity).
 *
 * Focus is one continuous loop of three stops -- the status bar, Read, Planner --
 * wrapping at both ends (Left2/side Down forward, Left1/side Up the reverse).
 * Right2 acts on the focused stop: Sync All on the status bar, otherwise opens the
 * tile. Right1 is Settings from anywhere on this screen, since there is nowhere
 * left to "go back" to -- this screen is Home.
 *
 * The tile artwork is the 80px Read and Tasks line icons the old home grid used,
 * drawn larger: the bitmaps are scaled up with bilinear sampling and thresholded
 * back to one bit (see drawScaledIcon() in the .cpp) rather than doubled pixel for
 * pixel, which would show every stair-step of the original.
 */
// Which area of the app the user was last in -- what Home puts focus back on. The Read
// menu and everything under it (Recent Books, the File Browser, the readers) are the
// Reader; the Planner screen is the Planner. Kept in RAM only.
enum class HomeArea : uint8_t { Reader, Planner };

class MainMenuActivity final : public Activity {
 public:
  // Called when the Read menu or the Planner screen opens (including through the side
  // buttons, which skip the main menu), so that Home from anywhere in either lands on
  // its own tile.
  static void setReturnArea(const HomeArea area) { returnArea = area; }
  explicit MainMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MainMenu", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  // The stops of the focus loop, in visual top-to-bottom order. Band is the whole
  // top of the screen -- the status bar, today's calendar events and the newest
  // pending alert, each its own stop (bandStop says which) -- then the two tiles.
  enum class Stop : uint8_t { Band, Read, Planner };

  inline static HomeArea returnArea = HomeArea::Reader;

  void activateFocused();
  // Geometry shared by render() and the touch hit-testing in loop(), so the two
  // can never disagree about where a tile is. The two options are drawn as boxes (the
  // selection outline's size, whether or not one is selected) with the same gap above
  // the first, between them, and below the second, down to the front-button labels.
  struct TileLayout {
    int iconSize;      // edge of the (square) icon
    int boxHeight;     // each option's box: icon, label and the padding around both
    int gap;           // the equal gap above, between and below
    int firstBoxTop;   // y of the first box's top edge
    int secondBoxTop;  // y of the second box's top edge
  };
  TileLayout tileLayout() const;

  ButtonNavigator buttonNavigator;
  Stop focus = Stop::Read;
  StatusBand band;
  StatusBand::Stop bandStop = StatusBand::Stop::StatusBar;
  // Home can be entered while Right1 is still held (e.g. leaving Settings with
  // it): ignore that stale release until a fresh press is seen here.
  bool settingsPressSeen = false;
  // Right2 can likewise still be down from the press that opened this screen.
  bool swallowConfirmRelease = false;
};
