#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

namespace {
// Floor a synced system clock could plausibly report; the un-synced default
// (seconds since boot, counted from the Unix epoch) reads as 1970 and is
// always well below this, so it is a reliable "has this actually been set"
// check without depending on SNTP's own sync-status flag (see the comment on
// getUtcDateTime() for why that flag cannot be trusted here).
constexpr int MIN_PLAUSIBLE_YEAR = 2024;
}  // namespace

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

namespace {
// UTC calendar date -> Unix epoch, no timezone involvement (newlib has no
// timegm). Days-from-civil per Howard Hinnant's algorithm.
time_t epochFromUtc(const Rtc::DateTime& dt) {
  int y = dt.year;
  const int m = dt.month;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + dt.day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  return static_cast<time_t>(days) * 86400 + dt.hour * 3600L + dt.minute * 60L + dt.second;
}
}  // namespace

void HalClock::setTimezone(const char* posixTz) {
  setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
  tzset();
  _lastPollMs = 0;  // re-derive local time under the new rule immediately
}

bool HalClock::utcEpoch(time_t& out) const {
  if (_available) {
    const unsigned long now = millis();
    if (_lastPollMs == 0 || (now - _lastPollMs) >= CLOCK_POLL_MS) {
      Rtc::DateTime dt;
      if (_sdkRtc.now(dt)) {
        _cachedUtc = epochFromUtc(dt);
        _hasCachedTime = true;
      } else if (!_hasCachedTime) {
        return false;
      }
      _lastPollMs = now != 0 ? now : 1;  // 0 doubles as the invalidation sentinel
    }
    out = _cachedUtc;
    return true;
  }

  // Boards without a hardware RTC (the X3/X4 here) only have the system clock,
  // set once via SNTP. Same plausible-year check as getUtcDateTime(); see its
  // comment for why SNTP's own sync flag is not used.
  const time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  if (utc.tm_year + 1900 < MIN_PLAUSIBLE_YEAR) return false;
  out = now;
  return true;
}

bool HalClock::localTime(struct tm& out) const {
  time_t utc;
  if (!utcEpoch(utc)) return false;
  localtime_r(&utc, &out);
  return true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  struct tm local;
  if (!localTime(local)) return false;
  hour = static_cast<uint8_t>(local.tm_hour);
  minute = static_cast<uint8_t>(local.tm_min);
  return true;
}

uint8_t HalClock::utcOffsetQuarterHoursBiased() const {
  time_t utc;
  if (!utcEpoch(utc)) return 48;  // unknown clock: UTC+0
  // newlib's struct tm has no tm_gmtoff: derive the offset from the local and UTC broken-down times.
  struct tm local;
  struct tm gm;
  localtime_r(&utc, &local);
  gmtime_r(&utc, &gm);
  int dayDelta = local.tm_yday - gm.tm_yday;
  if (dayDelta > 1) dayDelta = -1;  // local is still in the previous year
  if (dayDelta < -1) dayDelta = 1;  // local is already in the next year
  const int offsetMinutes = dayDelta * 1440 + (local.tm_hour - gm.tm_hour) * 60 + (local.tm_min - gm.tm_min);
  int quarters = offsetMinutes / 15 + 48;
  if (quarters < 0) quarters = 0;
  if (quarters > 104) quarters = 104;
  return static_cast<uint8_t>(quarters);
}

bool HalClock::getUtcDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
  // Reads UTC from the hardware RTC when present, else from the system clock once
  // SNTP has set it (a plausible year is what distinguishes "really set" from the
  // un-synced post-boot 1970 default; sntp_get_sync_status() lives in normal RAM,
  // which a deep-sleep wake resets to "unsynced" even though time() survives).
  time_t utc;
  if (!utcEpoch(utc)) return false;
  struct tm timeinfo;
  gmtime_r(&utc, &timeinfo);
  year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
  month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
  day = static_cast<uint8_t>(timeinfo.tm_mday);
  hour = static_cast<uint8_t>(timeinfo.tm_hour);
  minute = static_cast<uint8_t>(timeinfo.tm_min);
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  struct tm local;
  if (!localTime(local)) return false;

  if (use12Hour) {
    const bool pm = local.tm_hour >= 12;
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, local.tm_min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return true;
}

bool HalClock::formatHourMinute(char* buf, size_t bufSize, const uint8_t hour, const uint8_t minute,
                                uint8_t utcOffsetQuarterHoursBiased, const bool use12Hour) {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  // configTzTime overwrites the process TZ with UTC0 for the SNTP exchange;
  // remember the display timezone so it can be restored below.
  const char* tzBefore = getenv("TZ");
  char savedTz[64] = {0};
  if (tzBefore) snprintf(savedTz, sizeof(savedTz), "%s", tzBefore);
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (!_available) {
        // No hardware RTC to write: configTzTime() above already set the
        // system clock, which is the only UTC source utcEpoch()/getUtcDateTime()
        // read from on these boards (see their fallback comment).
        LOG_INF("CLK", "System clock synced to %04u-%02u-%02u %02u:%02u:%02u UTC", timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        setTimezone(savedTz);
        return true;
      }

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      const bool ok = _sdkRtc.set(dt);
      if (ok) {
        _cachedUtc = epochFromUtc(dt);
        _hasCachedTime = true;
        _lastPollMs = 0;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
      }
      setTimezone(savedTz);
      return ok;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  setTimezone(savedTz);
  return false;
}
