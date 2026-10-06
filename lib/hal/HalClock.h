#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  // The RTC keeps UTC; local time comes from newlib's localtime_r under the
  // POSIX TZ rule set via setTimezone(), so zones with DST are correct
  // year-round. Cached as a UTC epoch to keep the RTC bus quiet.
  mutable time_t _cachedUtc = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  // UTC epoch from the RTC (cached) or, without one, the system clock.
  bool utcEpoch(time_t& out) const;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Set the POSIX TZ rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") applied to every
  // read. nullptr/empty falls back to UTC. Drops the read cache so the change
  // shows immediately.
  void setTimezone(const char* posixTz);

  // Current wall-clock time in the configured timezone. On boards without a
  // hardware RTC (the X3/X4 here) it comes from the system clock once SNTP has set it.
  // Returns false if no usable time exists.
  bool localTime(struct tm& out) const;

  // Get current local hour (0-23) and minute (0-59).
  // Returns false if no usable time exists.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Full UTC wall-clock reading, including the calendar date.
  //
  // Unlike getTime() this is not shifted into the configured timezone: callers
  // doing date arithmetic (streaks, the companion's mood) need a date that does
  // not depend on a display setting. False until a usable clock exists.
  bool getUtcDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;

  // The configured timezone's current offset from UTC, in the quarter-hour units
  // (biased by 48, so 48 == UTC+0, range 0..104) the planner's date and time
  // maths use. DST-aware; 48 while the clock is unknown.
  uint8_t utcOffsetQuarterHoursBiased() const;

  // Format the local time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, bool use12Hour = false) const;

  // Same formatting/offset math as formatTime(), for an arbitrary UTC
  // hour/minute rather than the current reading -- for a caller displaying a
  // computed time (a countdown's end time) rather than "now". Same buffer
  // sizing and offset convention as formatTime(); does not touch the RTC, so
  // it needs no instance. Returns false (buf untouched) only if bufSize is
  // too small.
  static bool formatHourMinute(char* buf, size_t bufSize, uint8_t hour, uint8_t minute,
                               uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false);

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();
};
