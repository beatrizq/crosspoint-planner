#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

/**
 * The top of a browsing screen: the status bar (the theme's clock/date/battery
 * row), then today's calendar events, then the newest pending BLE alert -- in that
 * order, each part present only when there is something to show. Used by the
 * main menu, the Read menu, Recent Books, the File Browser, Settings and the
 * Planner; it is never part of the reader itself. Only the Planner screen turns the
 * events and the alert on (setShowCalendar / setShowAlert); everywhere else the band
 * is just the status bar.
 *
 * Every part is a focus stop on the screen that hosts it:
 *   - StatusBar: Right2 is Sync All and Right1 is Home (or whatever that screen
 *     already does there) -- those stay with the host screen.
 *   - Calendar: Right1 opens the Calendar screen.
 *   - Alert: Right2 dismisses it (the next-newest takes its place), Right1 opens
 *     the Alerts screen.
 * The host keeps its own "focus is up here" flag and adds a Stop to say which.
 *
 * An instance holds only what the host needs between frames: which parts were on
 * screen when it last drew (so focus can be walked and kept valid without
 * recomputing them), where the band ended, and the change-detection state behind
 * poll().
 */
class StatusBand {
 public:
  enum class Stop : uint8_t { None, StatusBar, Calendar, Alert };

  // Today's calendar events, one per row, in the calendar's own order -- with the
  // "never synced" line (isFallback) only when `includeSyncHint` is set.
  struct EventRow {
    std::string title;
    std::string time;  // ", HH:MM" (pre-formatted, own leading separator), or empty.
    bool isFallback = false;
    bool isNone = false;  // the "No events" line: synced, but nothing left today
  };
  static std::vector<EventRow> todaysEvents(bool includeSyncHint);

  static size_t pendingAlertCount();
  // Writes pending alert k's (0 = newest) "title: content" line; false if out of range.
  static bool pendingAlertLine(size_t k, char* out, size_t outSize);

  // Whether the Planner's "calendar never synced" line counts as a row. Only the
  // Planner sets it; elsewhere it would be a permanent line on every screen for
  // anyone without a calendar.
  void setIncludeSyncHint(const bool include) { includeSyncHint = include; }

  // Hides one part of the band on a screen that already shows it in its own way: the
  // Calendar screen lists the events itself, so it keeps only the alert; the Alerts
  // screen lists the alerts, so it keeps only the calendar rows. A hidden part is not
  // drawn and is not a focus stop.
  void setShowCalendar(const bool show) {
    showCalendar = show;
    if (!show) eventRowCount = 0;
  }
  void setShowAlert(const bool show) {
    showAlert = show;
    if (!show) alertCount = 0;
  }

  // Turns the band off for screens that only sometimes carry it (the File Browser
  // outside book browsing, Settings inside a category): the status bar and title
  // draw exactly as the theme always drew them, and there are no calendar or alert
  // stops.
  void setEnabled(const bool on) {
    enabled = on;
    if (!on) {
      eventRowCount = 0;
      alertCount = 0;
    }
  }

  // Draws, from `top`, the status bar (`statusRowHeight` tall, with its focus
  // highlight), the calendar rows and the alert, for a screen with no title of
  // its own. Returns the y just below the band.
  int drawUntitled(const GfxRenderer& renderer, int top, int statusRowHeight, Stop focus);

  // Draws the top of a screen that has a title under its status bar: the status
  // bar, the calendar rows and the alert, then the title and its rule. With
  // nothing to show in the band this is exactly the theme's own single header, so
  // the screen looks as it always did. Returns the y just below the title header
  // (where the screen's own spacing and content begin).
  int drawTitled(const GfxRenderer& renderer, const char* title, const char* subtitle, Stop focus);

  // Where the last draw ended -- for a host's touch hit-testing, which must agree
  // with what is on screen. Before the first draw: the theme's plain header.
  int bottom(const GfxRenderer& renderer) const;

  // Focus walking. Stops that are not on screen (no events, no alert) are
  // skipped. step() moves `stop` one stop in the given direction and returns
  // true, or -- when that would leave the band -- sets it to Stop::None and
  // returns false (the host then moves into its own content).
  Stop first() const { return Stop::StatusBar; }
  Stop last() const;
  bool step(Stop& stop, bool forward) const;
  // A stop that has since gone away (the last alert was dismissed) falls back to
  // the nearest one that is still there.
  Stop clamp(Stop stop) const;

  // True when what the band shows has changed since the last call (an alert
  // arrived or was dismissed elsewhere, an event ended) and the host should
  // repaint. Cheap: the alert count every call, the event list once a minute.
  bool poll();

  // Right1 / Right2 while `stop` has focus. They return true when the band
  // handled the press; false (StatusBar) leaves it to the host. handleRight2 on
  // the alert dismisses it, so the caller should then clamp its stop and repaint.
  bool handleRight1(Stop stop) const;
  bool handleRight2(Stop stop) const;
  // Button labels for a band stop (empty when there is nothing to offer).
  const char* right1Label(Stop stop) const;
  const char* right2Label(Stop stop) const;

 private:
  bool includeSyncHint = false;
  bool enabled = true;
  // Off unless the host asks: only the Planner screen shows today's events and the newest alert.
  bool showCalendar = false;
  bool showAlert = false;
  // What the last draw (or poll) found on screen.
  mutable int eventRowCount = 0;
  mutable size_t alertCount = 0;
  mutable int lastBottom = -1;
  unsigned long lastEventCheckMs = 0;
  bool polled = false;

  bool hasCalendar() const { return enabled && showCalendar && eventRowCount > 0; }
  bool hasAlert() const { return enabled && showAlert && alertCount > 0; }
};
