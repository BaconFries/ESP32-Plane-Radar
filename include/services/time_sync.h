#pragma once

#include <cstdint>

namespace services::time_sync {

/** Start NTP (UTC). Call once after Wi‑Fi connects. */
void begin();

/**
 * Apply local offset from Open-Meteo (or similar) utc_offset_seconds.
 * Reconfigures local time for getLocalTime().
 */
void setUtcOffsetSeconds(int32_t offset_sec);

int32_t utcOffsetSeconds();

/** True after at least one successful NTP sync. */
bool isSynced();

}  // namespace services::time_sync
