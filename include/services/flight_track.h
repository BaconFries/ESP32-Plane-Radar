#pragma once

#include <cstddef>
#include <cstdint>

namespace services::flight_track {

enum class Phase : uint8_t {
  None = 0,
  Searching,
  Airborne,
  OnGround,
  Lost,
};

struct Status {
  bool active = false;
  Phase phase = Phase::None;
  /** What the user entered (e.g. DL2460) — shown in UI. */
  char callsign[9] = "";
  /** ICAO-style id for ADS-B lookup when different (e.g. DAL2460). */
  char adsb_callsign[9] = "";
  char airline[28] = "";
  char origin_iata[4] = "";
  char dest_iata[4] = "";
  char route_line[24] = "";  // e.g. "SFO → SIN"
  char type[5] = "";
  char alt[12] = "";
  /** Local-time labels (e.g. "3:15 PM"); empty if unknown. */
  char takeoff_label[10] = "";
  char landing_label[10] = "";
  char eta_label[10] = "";
  float lat = 0.0f;
  float lon = 0.0f;
  float track_deg = 0.0f;
  float gs_knots = 0.0f;
  float dist_km = 0.0f;  // from radar center; -1 if unknown
  float dest_lat = 0.0f;
  float dest_lon = 0.0f;
  bool has_dest = false;
  bool has_position = false;
  bool route_ok = false;
  bool have_takeoff = false;
  bool have_landing = false;
  unsigned long last_seen_ms = 0;
  char phase_label[16] = "";
};

/** Load any persisted callsign from NVS. */
void init();

const Status& status();

/** True while a callsign is being tracked (until cleared / auto-ended). */
bool isActive();

/** Normalized callsign shown in UI (user entry); empty if none. */
const char* callsign();

/** Callsign used for ADS-B queries (may be ICAO form). */
const char* adsbCallsign();

/**
 * Start tracking a flight number / ADS-B callsign (e.g. UAL123 or UA123).
 * Fetches route metadata when possible. Returns false if callsign invalid.
 */
bool start(const char* flight_or_callsign);

/** Stop tracking and clear NVS. */
void clear();

/**
 * Refresh live position: match local ADS-B list first, else adsb.fi callsign API.
 * Respects rate limits; call from the main loop on a timer.
 * May auto-clear when the flight appears ended.
 */
void pollUpdate(double center_lat, double center_lon);

/** Short label for Phase (for UI). */
const char* phaseLabel(Phase phase);

}  // namespace services::flight_track
