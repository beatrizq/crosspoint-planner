#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Bitmap;
class GfxRenderer;
struct RecentBook;
namespace freeink {
namespace ui {
struct HeaderProps;
}  // namespace ui
}  // namespace freeink

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct TabInfo {
  const char* label;
  bool selected;
};

// Height a "focused header" invert-highlight uses, on every screen that has
// one -- deliberately independent of that screen's own (taller)
// metrics.headerHeight, which is sized for a full title+subtitle band.
// Inverting the whole band reads as a big, blocky fill; this hugs just the
// title row instead, the same thinness QuickPickActivity's own header focus
// originated with (its HEADER_HIGHLIGHT_HEIGHT), so every screen's header
// highlight reads as the same weight regardless of how tall its own header
// actually is.
constexpr int HEADER_FOCUS_HIGHLIGHT_HEIGHT = 30;

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  // FreeInkUI list shape, consumed by uiThemeTokens() for screens rendered
  // through FreeInkApp: the theme supplies geometry and selection style, the
  // uiScale fonts supply the sizes. Plain data by design — the eventual
  // SD-card theme files will provide exactly these values.
  int listRowGap;          // vertical gap between rows
  int listRowRadius;       // row corner radius (RoundedRaff cards, Lyra pill)
  int listInset;           // horizontal inset of the whole list band
  int listSidePadding;     // text inset within a row
  int listSelectionStyle;  // 0=invert fill, 1=light pill, 2=underline, 3=triangle (fui::SelectionStyle order)
  int listScrollWidth;     // scroll indicator thickness
  int listScrollSide;      // 0 = right edge, 1 = left edge
  bool listTitleBold;      // bold row titles (RoundedRaff)
  // FreeInkUI header shape, same contract as the list fields above.
  int headerSidePadding;    // title text inset
  int headerUnderlineSize;  // bottom rule thickness (Lyra), 0 = none
  int headerTitleAlign;     // 0 = left, 1 = center, 2 = right (fui::TextAlign order)
  int headerBatterySide;    // 0 = right edge, 1 = left edge
  // Header clock opt-out for themes whose title layout can't spare the left
  // reserve (RoundedRaff); the user setting still governs the themes that can.
  bool headerShowsClock = true;
  // Clock slot: centered on the band, or on the left after the back arrow.
  bool headerClockCentered = true;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;
  int coverGridTabBarHeight = 72;
  // Selected-tab pill fills its equal-width slot (legacy RoundedRaff tabs)
  // instead of shrinking to hug the label (legacy Lyra tabs).
  bool tabPillFullSlot = false;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  // False when a theme's own drawRecentBookCover() has moved off Home
  // entirely (see ReadMenuActivity, which calls the same per-theme function
  // for its own leading row instead) -- distinct from
  // homeContinueReadingInMenu, which is about whether the *menu* carries a
  // "Read"/"Continue reading" entry, not about whether the cover card itself
  // draws.
  bool homeShowsCoverCard;
  int homeMenuTopOffset;
  // Home menu as tiles rather than rows: columns across, and the height of one
  // tile including its label. Zero columns keeps the list.
  //
  // The column count decides the row count too - the entries wrap into
  // ceil(count / columns) rows, which then share whatever height is left under
  // the cover card. Both the drawing and the tile hit-testing read this one
  // value, so they cannot disagree about the shape.
  int homeGridColumns;
  int homeGridTileHeight;

  int buttonHintsHeight;
  // Vertical gap between the bottom of a screen's content and the front button hints.
  int buttonHintsGap;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int optionPopupItemSpacing;
  int optionPopupInnerPadding;
  int optionPopupSelectionVPadding;
  int optionPopupDialogSideMargin;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;

  // FreeInkUI control shape (the control center panel), same contract as the
  // list fields above: quick-setting tiles and slider step buttons, the
  // sheet's free-edge corners, and the capsule slider's corners (255 = full
  // stadium, i.e. radius = half the control height).
  int controlRadius;
  int sheetRadius;
  int capsuleRadius;
};

enum UIIcon {
  None = 0,
  Folder,
  Text,
  Image,
  Book,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Wifi,
  Hotspot,
  Bookmark,
  Usb,
  Blocks,
  Tasks,
  Calendar,
  Bell
};

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .listRowGap = 0,
                                 .listRowRadius = 0,
                                 .listInset = 0,
                                 .listSidePadding = 20,
                                 .listSelectionStyle = 0,  // invert fill
                                 .listScrollWidth = 4,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .headerSidePadding = 18,
                                 .headerUnderlineSize = 0,
                                 .headerTitleAlign = 1,  // centered
                                 .headerBatterySide = 0,
                                 // Corner clock: a centered clock would collide with the centered title.
                                 .headerClockCentered = false,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeShowsCoverCard = true,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 40,
                                 .buttonHintsGap = 8,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionVPadding = 4,
                                 .optionPopupDialogSideMargin = 20,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0,
                                 .controlRadius = 0,
                                 .sheetRadius = 0,
                                 .capsuleRadius = 0};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  static void drawCoverPlaceholder(const GfxRenderer& renderer, Rect rect);
  // Draws a pre-dithered cover thumb 1:1, centered and clipped to fill the
  // slot. Rescaling a dithered bitmap aliases badly, so overflow is cropped.
  static bool drawCoverThumbFill(const GfxRenderer& renderer, const Bitmap& bitmap, Rect slot, int xOffset = 0);
  static void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total);
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        bool showPercentage = true) const;  // Right aligned (headers)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  // Shared by every theme's drawButtonHints(): centres a hint label in its box,
  // wrapping to two lines rather than overflowing when it's too wide to fit.
  static void drawHintLabel(const GfxRenderer& renderer, int fontId, const char* label, int x, int boxWidth, int boxTop,
                            int boxHeight, int singleLineYOffset);
  // topSelected/bottomSelected draw that label inverted (white letters on a
  // black fill) -- how a caller marks which of the two is the current
  // selection.
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                                   bool topSelected = false, bool bottomSelected = false) const;
  // The y just below the lowest side-button box drawSideButtonHints() draws for
  // these two labels -- how far down a screen must start its content so the
  // labels never sit beside it. 0 when there is no such line: nothing is drawn
  // (touch devices), or the two buttons aren't at one shared height (the X4).
  virtual int getSideButtonHintsBottom(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  // Menu row height as DRAWN by drawButtonMenu. HomeActivity builds its touch
  // grid from this, so hit bands always match the visuals (RoundedRaff derives
  // its row height from the font, not the metrics table).
  virtual int getMenuRowHeight(const GfxRenderer& renderer) const;
  virtual int getListRowStep(bool hasSubtitle) const;
  virtual int getListPageItems(int contentHeight, bool hasSubtitle) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        const std::function<bool(int index)>& rowDimmed = nullptr) const;
  // Also draws the wall clock opposite the battery when the user enabled
  // SETTINGS.clockShowInHeader and an RTC is present. On touch boards a
  // tappable back button leads the band (see HeaderBackTapTarget); root
  // screens that own their stack bottom pass backButton = false.
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                          bool backButton = true) const;
  // The pre-FreeInkUI header: the Planner's status band (StatusBand) is laid out
  // around its pixel geometry, so it keeps drawing it rather than drawHeader's.
  // showRule: whether to draw the thin rule under the header, on the one theme
  // (Lyra) that draws one -- false for the band, whose focus highlight sits where
  // that rule would. includeStatusRow: whether to draw the clock/date/battery row
  // a theme otherwise always paints into a header rect -- false for a *second*,
  // embedded title drawn mid-screen (the band's title under its own status row),
  // so that row isn't duplicated.
  virtual void drawLegacyHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                                const char* subtitle = nullptr, bool showRule = true,
                                bool includeStatusRow = true) const;
  // Fill the battery/clock status chrome (settings + theme metrics) into
  // header props, so FUI-native screens drawing their own interactive header
  // carry the same band as drawHeader. Status text is styled with the
  // FONT_LABEL slot (bound to the fixed small font by makeUiTarget and
  // drawHeader). The label strings point at internal static buffers refreshed
  // per call (headers draw on the single render task).
  static void applyHeaderStatus(const GfxRenderer& renderer, freeink::ui::HeaderProps& props);
  // Edge inset drawHeader uses for the clock/battery status line (detached
  // layouts hug the corner with a legacy 12px inset instead of the padding).
  static int headerStatusInset();
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                                 int& index) const;

  // The run of tabs a bar can actually show. The Calendar screen draws one tab
  // per selected calendar, so a bar can be asked for more tabs than fit across
  // the screen; rather than clipping the overflow silently, each theme draws a
  // window around the active tab and marks the directions it has more in.
  struct TabWindow {
    int first;        // Index of the first tab to draw
    int count;        // How many to draw, starting at first
    bool moreBefore;  // Tabs exist to the left of the window
    bool moreAfter;   // Tabs exist to the right of the window
  };

  // Marker drawn where the window has more tabs beyond it. ASCII, because the UI
  // fonts are subset and cannot be relied on for the typographic guillemets.
  static constexpr const char* TAB_MORE_BEFORE = "<";
  static constexpr const char* TAB_MORE_AFTER = ">";

  /**
   * Picks the window to draw. `widths` holds each tab's full advance - its own
   * width plus the spacing that follows it - because only the theme knows how it
   * lays a tab out. `available` is the room the bar has for tabs, and `reserve`
   * the room the two markers need, taken off only when the tabs do not all fit.
   *
   * The window grows outward from `active`, right first so the bar reads
   * left-to-right, and always contains the active tab even when that one tab is
   * wider than the bar.
   */
  static TabWindow tabWindow(const std::vector<int>& widths, int available, int reserve, int active);

  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  // The same entries as tiles: artwork above a centred label, the selected one
  // boxed. Themes that do not draw a grid inherit the list instead.
  // Vertical step between tile rows, given the height the grid has to fill and
  // how many tiles go in it. The activity hit-tests with this too.
  virtual int getGridRowStep(int contentHeight, int buttonCount) const;
  // badgeCount, when given, returns a notification-style count for a tile (0 =
  // no badge). A theme that has no room to draw one - anything falling back to
  // drawButtonMenu's row list - is free to ignore it.
  virtual void drawButtonGrid(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon,
                              const std::function<int(int index)>& badgeCount = nullptr) const;
  // The exact rect drawButtonGrid() would draw tile `index`'s icon into, for a
  // caller that wants to overlay its own artwork there instead of a static
  // UIIcon (the companion's home tile draws its own current pose here, sized
  // to fit whatever this returns, rather than an icon bitmap). An empty rect
  // means this theme has no grid to overlay onto (drawButtonGrid() falls back
  // to the plain row list, which has no icon-sized square of its own).
  virtual Rect getGridTileIconRect(const GfxRenderer& renderer, Rect rect, int buttonCount, int index) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  static void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                            std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                            const bool fillMargin = true, const bool isPageBookmarked = false,
                            const bool pageCountEstimated = false);
  static void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label);
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }
  // Thumb generation height for home covers; 0 means use metrics.homeCoverHeight.
  // Themes with slots wider than 0.6 aspect override this so covers still fill.
  virtual int homeCoverThumbHeight(const GfxRenderer&) const { return 0; }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};
