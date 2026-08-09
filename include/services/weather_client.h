#pragma once

#include <cstdint>

namespace services::weather {

struct Conditions {
  bool valid = false;
  float temp_c = 0.0f;
  int weather_code = 0;
  char summary[16] = "";
  int32_t utc_offset_sec = 0;
  unsigned long fetched_ms = 0;
};

/** Last successful fetch (may be stale). */
const Conditions& current();

/** Short WMO weather_code label for the display. */
const char* summaryForCode(int weather_code);

/**
 * Fetch current conditions from Open-Meteo (HTTP).
 * Updates timezone offset via time_sync when successful.
 */
bool fetchUpdate(double lat, double lon);

/** True if we should refresh (never fetched or older than interval). */
bool needsRefresh(unsigned long interval_ms);

}  // namespace services::weather
