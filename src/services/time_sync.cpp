#include "services/time_sync.h"

#include <cstdlib>
#include <ctime>

#include <Arduino.h>

namespace services::time_sync {

namespace {

int32_t s_offset_sec = 0;
bool s_begun = false;
unsigned long s_last_retry_ms = 0;

constexpr unsigned long kNtpRetryMs = 30000;

/**
 * Open-Meteo utc_offset_seconds is ISO-style (local = UTC + offset).
 * POSIX TZ uses the opposite sign (hours to add to local to get UTC).
 */
void applyPosixTz(int32_t utc_offset_sec) {
  const int32_t posix = -utc_offset_sec;
  const int hours = posix / 3600;
  int mins = static_cast<int>((posix < 0 ? -posix : posix) % 3600) / 60;
  char tz[24];
  if (mins == 0) {
    snprintf(tz, sizeof(tz), "UTC%+d", hours);
  } else {
    snprintf(tz, sizeof(tz), "UTC%+d:%02d", hours, mins);
  }
  setenv("TZ", tz, 1);
  tzset();
}

void startSntp() {
  // Keep SNTP in UTC; local wall time comes from TZ (applyPosixTz).
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  applyPosixTz(s_offset_sec);
}

}  // namespace

void begin() {
  s_begun = true;
  s_last_retry_ms = millis();
  startSntp();
  Serial.println("NTP: started (UTC until weather offset)");
}

void setUtcOffsetSeconds(int32_t offset_sec) {
  if (offset_sec == s_offset_sec && s_begun) {
    return;
  }
  s_offset_sec = offset_sec;
  // Do not call configTime() here — that stops/restarts SNTP and can leave the
  // clock stuck on "Syncing time…" while ADS-B HTTPS is already running.
  applyPosixTz(offset_sec);
  Serial.printf("NTP: local offset %+d s\n", static_cast<int>(offset_sec));
}

int32_t utcOffsetSeconds() { return s_offset_sec; }

bool isSynced() {
  struct tm ti;
  return getLocalTime(&ti, 0);
}

void retryIfNeeded() {
  if (!s_begun || isSynced()) {
    return;
  }
  const unsigned long now = millis();
  if (now - s_last_retry_ms < kNtpRetryMs) {
    return;
  }
  s_last_retry_ms = now;
  Serial.println("NTP: still not synced — restarting SNTP");
  startSntp();
}

}  // namespace services::time_sync
