#pragma once

namespace services::location {

/** Load saved lat/lon (and optional ZIP) from NVS, or use config defaults. */
void init();

/** Factory defaults when nothing is stored (also used for portal field prefill). */
double lat();
double lon();

/** Optional US ZIP used for portal prefill; empty string if unset. */
const char* zip();

/**
 * Parse portal strings, validate, persist to NVS, update runtime values.
 * A 5-digit US ZIP is preferred: device looks it up when on Wi‑Fi; otherwise
 * the portal page fills lat/lon in the browser before submit.
 * zip_str may be empty; non-empty values are stored for prefill.
 */
bool saveFromPortal(const char* lat_str, const char* lon_str, const char* zip_str);

/** Parse portal strings, validate, persist to NVS, update runtime values. */
bool saveFromStrings(const char* lat_str, const char* lon_str);

/** Clear stored coordinates and ZIP (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::location
