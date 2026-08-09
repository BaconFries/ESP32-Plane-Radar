#include "services/time_sync.h"

#include <time.h>

#include <Arduino.h>

namespace services::time_sync {

namespace {

int32_t s_offset_sec = 0;
bool s_begun = false;

}  // namespace

void begin() {
  // Start in UTC; weather fetch applies local offset.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  s_begun = true;
  s_offset_sec = 0;
  Serial.println("NTP: started (UTC until weather offset)");
}

void setUtcOffsetSeconds(int32_t offset_sec) {
  if (offset_sec == s_offset_sec && s_begun) {
    return;
  }
  s_offset_sec = offset_sec;
  configTime(offset_sec, 0, "pool.ntp.org", "time.nist.gov");
  Serial.printf("NTP: local offset %+d s\n", static_cast<int>(offset_sec));
}

int32_t utcOffsetSeconds() { return s_offset_sec; }

bool isSynced() {
  struct tm ti;
  return getLocalTime(&ti, 0);
}

}  // namespace services::time_sync
