#include "StatusBand.h"

#include <GCalEventCache.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CompanionMood.h"
#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "companion/CompanionRenderer.h"
#include "components/UITheme.h"
#include "components/icons/bell24.h"
#include "fontIds.h"
#include "util/HomeAppOrder.h"
#include "util/PlannerSync.h"

#ifdef ENABLE_BLE_NOTIFY_SPIKE
#include "network/BleNotificationQueue.h"
#endif

namespace {
// Height of the status bar's focus highlight below the top of its row: a little
// shorter than the row itself, which carries some breathing room underneath that
// the highlight would otherwise bleed into the next part.
// Height of the status row (clock/date/battery) on every screen with a band. MainMenuActivity's STATUS_BAR_HEIGHT
// and QuickPickActivity's HEADER_CONTENT_HEIGHT are the same 36: keep the three equal.
constexpr int UNTITLED_STATUS_ROW_HEIGHT = 36;
constexpr int HIGHLIGHT_INSET = 6;
// The highlight on a theme's own full header, which is taller than its clock/
// battery row (see HEADER_FOCUS_HIGHLIGHT_HEIGHT in BaseTheme.h).
constexpr int FULL_HEADER_HIGHLIGHT = HEADER_FOCUS_HIGHLIGHT_HEIGHT;

// Calendar rows: the gap between lines, and the breathing room under the last one.
constexpr int GLANCE_LINE_GAP = 4;
constexpr int GLANCE_STRIP_GAP = 10;
// The gap between a row's leading bullet/icon and its text.
constexpr int GLANCE_ICON_GAP = 8;
constexpr int BULLET_SIZE = 6;
// The x that stands in for the dot on the "No events" line: the same size as the dot.
constexpr int NONE_MARK_SIZE = BULLET_SIZE;

// The alert: padding above and below its text, how many lines it may wrap over,
// its bell icon (bell24.h is 24x24), and the gap under it before whatever follows.
constexpr int ALERT_PAD = 4;
constexpr int ALERT_MAX_LINES = 3;
constexpr int ALERT_ICON_SIZE = 24;
constexpr int ALERT_BOTTOM_GAP = 6;

constexpr unsigned long EVENT_CHECK_INTERVAL_MS = 60000;

// Dither-overlay "dim": this e-ink panel has no real greyscale, so dimmed text is
// solid text with a checkerboard of pixels punched back out over it. Call right
// after drawText() at the same position -- for an event's trailing time.
void dimText(const GfxRenderer& renderer, const int x, const int y, const int fontId, const char* text) {
  if (text == nullptr || text[0] == '\0') return;
  const int width = renderer.getTextWidth(fontId, text);
  const int height = renderer.getLineHeight(fontId);
  for (int py = y; py < y + height; py++) {
    for (int px = x; px < x + width; px++) {
      if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }
  }
}

// Draws a 1-bpp square icon (MSB first, bit 0 = ink -- the format
// GfxRenderer::drawIcon() takes) in black, or in white on a filled background.
// drawIcon() only inks black; this plots through the same orientation mapping so
// the two line up pixel for pixel.
void drawIconInk(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const int size,
                 const bool black) {
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      if (((byte >> (7 - (col & 7))) & 1) == 0) renderer.drawPixel(x + (size - 1 - row), y + col, black);
    }
  }
}

// The calendar rows, from `y`: one bulleted line per event, the trailing time
// dimmed. Returns the height used (the rows plus GLANCE_STRIP_GAP below them).
// Inverted over its whole height when `focused`.
int drawCalendarRows(const GfxRenderer& renderer, const int y, const std::vector<StatusBand::EventRow>& rows,
                     const bool focused) {
  if (rows.empty()) return 0;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int count = static_cast<int>(rows.size());
  const int textHeight = count * lineH + (count - 1) * GLANCE_LINE_GAP;
  const int height = textHeight + GLANCE_STRIP_GAP;
  // The focus invert spans the full height, so the text is nudged down by half
  // the trailing gap, splitting it evenly above and below the glyphs.
  const int bulletX = metrics.contentSidePadding;
  const int textX = bulletX + BULLET_SIZE + GLANCE_ICON_GAP;
  const int maxWidth = pageWidth - metrics.contentSidePadding * 2 - BULLET_SIZE - GLANCE_ICON_GAP;
  int rowY = y + (height - textHeight) / 2;
  for (const auto& row : rows) {
    // No bullet for a fallback line (not synced yet): it isn't an event.
    if (row.isNone) {
      // An x where an event has its dot.
      const int markY = rowY + (lineH - NONE_MARK_SIZE) / 2;
      renderer.drawLine(bulletX, markY, bulletX + NONE_MARK_SIZE - 1, markY + NONE_MARK_SIZE - 1, 2, true);
      renderer.drawLine(bulletX + NONE_MARK_SIZE - 1, markY, bulletX, markY + NONE_MARK_SIZE - 1, 2, true);
    } else if (!row.isFallback) {
      renderer.fillRoundedRect(bulletX, rowY + (lineH - BULLET_SIZE) / 2, BULLET_SIZE, BULLET_SIZE, BULLET_SIZE / 2,
                               Color::Black);
    }
    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, row.title.c_str(), maxWidth);
    renderer.drawText(UI_10_FONT_ID, textX, rowY, shown.c_str());
    // The time rides next to the title, dimmed, only when the title was not cut
    // and only as much of it as still fits.
    if (!row.time.empty() && shown == row.title) {
      const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, shown.c_str());
      const int room = maxWidth - titleWidth;
      if (room > 0) {
        const int timeX = textX + titleWidth;
        const std::string timeShown = renderer.truncatedText(UI_10_FONT_ID, row.time.c_str(), room);
        renderer.drawText(UI_10_FONT_ID, timeX, rowY, timeShown.c_str());
        dimText(renderer, timeX, rowY, UI_10_FONT_ID, timeShown.c_str());
      }
    }
    rowY += lineH + GLANCE_LINE_GAP;
  }
  if (focused) renderer.invertRect(0, y, pageWidth, height);
  return height;
}

// The newest pending alert, from `y`: its bell, then its text wrapped over up to
// ALERT_MAX_LINES lines. Returns the height used (including ALERT_BOTTOM_GAP), or 0
// when there is none. Drawn white on black when `focused`.
int drawAlert(const GfxRenderer& renderer, const int y, const bool focused) {
  char line[192];
  if (!StatusBand::pendingAlertLine(0, line, sizeof(line))) return 0;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int iconX = metrics.contentSidePadding;
  const int textX = iconX + ALERT_ICON_SIZE + GLANCE_ICON_GAP;
  const int maxWidth = pageWidth - metrics.contentSidePadding * 2 - ALERT_ICON_SIZE - GLANCE_ICON_GAP;
  const auto fit =
      companion::fitBubbleText(renderer, UI_10_FONT_ID, line, maxWidth, /*minTextWidth=*/0, ALERT_MAX_LINES);
  const int textHeight = static_cast<int>(fit.lines.size()) * lineH;
  const int blockHeight = std::max(textHeight, ALERT_ICON_SIZE) + ALERT_PAD * 2;
  if (focused) renderer.fillRect(0, y, pageWidth, blockHeight);
  int textY = y + (blockHeight - textHeight) / 2;
  // The bell sits on the first line of text, its centre on that line's centre.
  drawIconInk(renderer, Bell24Icon, iconX, textY + (lineH - ALERT_ICON_SIZE) / 2, ALERT_ICON_SIZE, !focused);
  for (const auto& wrapped : fit.lines) {
    renderer.drawText(UI_10_FONT_ID, textX, textY, wrapped.c_str(), !focused);
    textY += lineH;
  }
  return blockHeight + ALERT_BOTTOM_GAP;
}
}  // namespace

std::vector<StatusBand::EventRow> StatusBand::todaysEvents(const bool includeSyncHint) {
  std::vector<EventRow> rows;
  const uint16_t today = plannerSync::todayLocalDate();
  // No usable "today" to filter by is no more useful than a never-synced cache:
  // either way this cannot promise anything about what is on today.
  if (!GCAL_EVENTS.hasSynced() || today == civil::NO_DATE) {
    if (includeSyncHint) rows.push_back(EventRow{std::string(tr(STR_GCAL_NEVER_SYNCED)), "", true});
    return rows;
  }
  // Exact-day match: a sync can be hours or days old and today's date (re-derived
  // fresh here) can have moved on since, so an event dated "yesterday" in a stale
  // cache simply no longer matches.
  // An event that has already ended is not worth a row: once the local time is at
  // or past its end it is dropped. Only a timed event with a known end that lies
  // after its start can end today (an all-day one runs all day; an unknown end, or
  // an end at or before the start -- one that runs past midnight -- is kept rather
  // than guessed at), and only when the clock says what time it is.
  uint16_t nowMinute = civil::NO_TIME;
  {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (halClock.getUtcDateTime(year, month, day, hour, minute)) {
      // clockUtcOffsetQ is biased by 48 (48 == UTC+0), the same clamp and bias every
      // other local-time reader of it uses.
      const int32_t offsetQuarterHours =
          static_cast<int32_t>(std::min<uint8_t>(halClock.utcOffsetQuarterHoursBiased(), 104)) - 48;
      nowMinute = companion::localMinuteOfDay(hour, minute, offsetQuarterHours);
    }
  }
  for (const auto& event : GCAL_EVENTS.getEvents()) {
    if (event.date != today) continue;
    if (nowMinute != civil::NO_TIME && !event.isAllDay() && event.endMin != civil::NO_TIME &&
        event.endMin > event.startMin && nowMinute >= event.endMin) {
      continue;
    }
    EventRow row;
    row.title = event.summary;
    if (!event.isAllDay()) {
      char time[10];
      snprintf(time, sizeof(time), ", %02u:%02u", static_cast<unsigned>(event.startMin / 60),
               static_cast<unsigned>(event.startMin % 60));
      row.time = time;
    }
    rows.push_back(std::move(row));
  }
  // Synced with nothing left today: the Planner (the only caller that sets includeSyncHint) says so
  // with a line of its own; elsewhere the band just collapses.
  if (rows.empty() && includeSyncHint) {
    EventRow none{std::string(tr(STR_NO_EVENTS_TODAY)), "", false};
    none.isNone = true;
    rows.push_back(std::move(none));
  }
  return rows;
}

size_t StatusBand::pendingAlertCount() {
#ifdef ENABLE_BLE_NOTIFY_SPIKE
  return BLE_NOTIFICATIONS.getPendingCount();
#else
  return 0;
#endif
}

bool StatusBand::pendingAlertLine(const size_t k, char* out, const size_t outSize) {
#ifdef ENABLE_BLE_NOTIFY_SPIKE
  const auto* alert = BLE_NOTIFICATIONS.getPendingEntry(k);
  if (alert == nullptr) return false;
  if (alert->content[0] != '\0') {
    snprintf(out, outSize, "%s: %s", alert->title, alert->content);
  } else {
    snprintf(out, outSize, "%s", alert->title);
  }
  return true;
#else
  (void)k;
  (void)out;
  (void)outSize;
  return false;
#endif
}

int StatusBand::drawUntitled(const GfxRenderer& renderer, const int top, const int statusRowHeight, const Stop focus) {
  const int pageWidth = renderer.getScreenWidth();

  // The theme's own clock/date/battery row: a null title and subtitle drop the
  // title band and leave just that row (Lyra draws it unconditionally into
  // whatever rect it is given).
  GUI.drawLegacyHeader(renderer, Rect{0, top, pageWidth, statusRowHeight}, /*title=*/nullptr, /*subtitle=*/nullptr,
                       /*showRule=*/false, /*includeStatusRow=*/true);
  // Hovering it: a true pixel invert over the row, not a fill (which would paint
  // over what the theme just drew).
  if (focus == Stop::StatusBar) renderer.invertRect(0, top, pageWidth, statusRowHeight - HIGHLIGHT_INSET);
  int y = top + statusRowHeight;

  const auto rows = (enabled && showCalendar) ? todaysEvents(includeSyncHint) : std::vector<EventRow>{};
  eventRowCount = static_cast<int>(rows.size());
  y += drawCalendarRows(renderer, y, rows, focus == Stop::Calendar);

  alertCount = (enabled && showAlert) ? pendingAlertCount() : 0;
  if (alertCount > 0) y += drawAlert(renderer, y, focus == Stop::Alert);

  lastBottom = y;
  return y;
}

int StatusBand::drawTitled(const GfxRenderer& renderer, const char* title, const char* subtitle, const Stop focus) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int top = metrics.topPadding;

  const auto rows = (enabled && showCalendar) ? todaysEvents(includeSyncHint) : std::vector<EventRow>{};
  eventRowCount = static_cast<int>(rows.size());
  alertCount = (enabled && showAlert) ? pendingAlertCount() : 0;

  if (rows.empty() && alertCount == 0) {
    // Nothing to put between the status bar and the title: the theme's own single
    // header, exactly as the screen always drew it.
    const Rect headerRect{0, top, pageWidth, metrics.headerHeight};
    GUI.drawLegacyHeader(renderer, headerRect, title, subtitle);
    if (focus == Stop::StatusBar) {
      renderer.invertRect(headerRect.x, headerRect.y, headerRect.width,
                          std::min(FULL_HEADER_HIGHLIGHT, headerRect.height));
    }
    lastBottom = top + metrics.headerHeight;
    return lastBottom;
  }

  // The status bar, then the rows and the alert, then the title -- the title header
  // is the theme's own with its status row left out, which puts the title and its
  // rule exactly where the single header would have (batteryBarHeight + 3 down).
  // The same row height the untitled screens (main menu, companion) use, so the calendar rows and the
  // alert sit at the same y on every screen; the theme's batteryBarHeight is a little taller than the
  // clock/battery row actually needs.
  const int statusRowHeight = UNTITLED_STATUS_ROW_HEIGHT;
  GUI.drawLegacyHeader(renderer, Rect{0, top, pageWidth, statusRowHeight}, nullptr, nullptr, /*showRule=*/false,
                       /*includeStatusRow=*/true);
  if (focus == Stop::StatusBar) {
    renderer.invertRect(0, top, pageWidth, std::min(FULL_HEADER_HIGHLIGHT, statusRowHeight));
  }
  int y = top + statusRowHeight;
  y += drawCalendarRows(renderer, y, rows, focus == Stop::Calendar);
  y += drawAlert(renderer, y, focus == Stop::Alert);

  const int titleHeight = metrics.headerHeight - metrics.batteryBarHeight;
  GUI.drawLegacyHeader(renderer, Rect{0, y, pageWidth, titleHeight}, title, subtitle, /*showRule=*/true,
                       /*includeStatusRow=*/false);
  lastBottom = y + titleHeight;
  return lastBottom;
}

int StatusBand::bottom(const GfxRenderer&) const {
  if (lastBottom >= 0) return lastBottom;
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight;
}

StatusBand::Stop StatusBand::last() const {
  if (hasAlert()) return Stop::Alert;
  if (hasCalendar()) return Stop::Calendar;
  return Stop::StatusBar;
}

bool StatusBand::step(Stop& stop, const bool forward) const {
  // The stops that are on screen, top to bottom.
  Stop stops[3];
  int count = 0;
  stops[count++] = Stop::StatusBar;
  if (hasCalendar()) stops[count++] = Stop::Calendar;
  if (hasAlert()) stops[count++] = Stop::Alert;

  int index = 0;
  for (int i = 0; i < count; i++) {
    if (stops[i] == stop) index = i;
  }
  const int next = index + (forward ? 1 : -1);
  if (next < 0 || next >= count) {
    stop = Stop::None;
    return false;
  }
  stop = stops[next];
  return true;
}

StatusBand::Stop StatusBand::clamp(const Stop stop) const {
  if (stop == Stop::Alert && !hasAlert()) return hasCalendar() ? Stop::Calendar : Stop::StatusBar;
  if (stop == Stop::Calendar && !hasCalendar()) return Stop::StatusBar;
  return stop;
}

bool StatusBand::poll() {
  if (!enabled) return false;
  const size_t alerts = showAlert ? pendingAlertCount() : 0;
  const unsigned long now = millis();
  if (!polled) {
    // First look: just learn what is there -- the first draw is already due.
    polled = true;
    alertCount = alerts;
    eventRowCount = showCalendar ? static_cast<int>(todaysEvents(includeSyncHint).size()) : 0;
    lastEventCheckMs = now;
    return false;
  }
  bool changed = false;
  if (alerts != alertCount) {
    alertCount = alerts;
    changed = true;
  }
  // An event ending drops its row and nothing else would repaint for that, so the
  // list is re-checked on a slow timer rather than every frame.
  if (now - lastEventCheckMs >= EVENT_CHECK_INTERVAL_MS) {
    lastEventCheckMs = now;
    const int rows = showCalendar ? static_cast<int>(todaysEvents(includeSyncHint).size()) : 0;
    if (rows != eventRowCount) {
      eventRowCount = rows;
      changed = true;
    }
  }
  return changed;
}

bool StatusBand::handleRight1(const Stop stop) const {
  switch (stop) {
    case Stop::Calendar:
      activityManager.goToCalendar();
      return true;
    case Stop::Alert:
#ifdef ENABLE_BLE_NOTIFY_SPIKE
      activityManager.goToBleNotifications();
#endif
      return true;
    default:
      return false;
  }
}

bool StatusBand::handleRight2(const Stop stop) const {
  switch (stop) {
    case Stop::Calendar:
      return true;  // nothing to act on; consumed so it does not fall through
    case Stop::Alert:
#ifdef ENABLE_BLE_NOTIFY_SPIKE
      // Dismiss the alert on top; the next-newest takes its place.
      BLE_NOTIFICATIONS.dismissPending(0);
      alertCount = pendingAlertCount();
#endif
      return true;
    default:
      return false;
  }
}

const char* StatusBand::right1Label(const Stop stop) const {
  switch (stop) {
    case Stop::StatusBar:
      return tr(STR_HOME);
    case Stop::Calendar:
      return homeAppOrder::displayName(homeAppOrder::AppId::Calendar);
    case Stop::Alert:
#ifdef ENABLE_BLE_NOTIFY_SPIKE
      return homeAppOrder::displayName(homeAppOrder::AppId::Notifications);
#else
      return "";
#endif
    default:
      return "";
  }
}

const char* StatusBand::right2Label(const Stop stop) const {
  switch (stop) {
    case Stop::StatusBar:
      return tr(STR_SYNC_ALL);
    case Stop::Alert:
      return tr(STR_DISMISS);
    default:
      return "";
  }
}
