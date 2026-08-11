#include "services/flight_track.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "data/large_airports.h"
#include "services/adsb_client.h"
#include "services/wifi_setup.h"

namespace services::flight_track {

namespace {

constexpr char kPrefsNamespace[] = "flighttrk";
constexpr char kKeyCallsign[] = "cs";
constexpr char kKeyOrigin[] = "orig";
constexpr char kKeyDest[] = "dest";

/** Live position by callsign (same JSON shape: { "ac": [ ... ] }). */
constexpr const char* kLiveCallsignApis[] = {
    "https://opendata.adsb.fi/api/v2/callsign/",
    "https://api.airplanes.live/v2/callsign/",
    "https://api.adsb.lol/v2/callsign/",
};
constexpr size_t kLiveApiCount =
    sizeof(kLiveCallsignApis) / sizeof(kLiveCallsignApis[0]);

/** Airline + ICAO callsign only — OD pairs from this DB are often stale. */
constexpr char kRouteApi[] = "https://api.adsbdb.com/v0/callsign/";

constexpr float kKmPerDeg = 111.0f;
constexpr unsigned long kRequestTimeoutMs = 10000;

Status s_status{};
unsigned long s_started_ms = 0;
unsigned long s_last_poll_ms = 0;
unsigned long s_ground_since_ms = 0;
bool s_was_airborne = false;
/** After a completed landing: no more callsign HTTP until clear. */
bool s_post_landing_idle = false;

struct AirlineCode {
  const char iata[3];
  const char icao[4];
};

// Common IATA → ICAO airline prefixes for ADS-B callsign expansion.
constexpr AirlineCode kAirlineCodes[] = {
    {"AA", "AAL"}, {"AC", "ACA"}, {"AF", "AFR"}, {"AS", "ASA"}, {"B6", "JBU"},
    {"BA", "BAW"}, {"DL", "DAL"}, {"EK", "UAE"}, {"F9", "FFT"}, {"FI", "ICE"},
    {"G4", "AAY"}, {"HA", "HAL"}, {"IB", "IBE"}, {"KL", "KLM"}, {"LH", "DLH"},
    {"LX", "SWR"}, {"NK", "NKS"}, {"QF", "QFA"}, {"QR", "QTR"}, {"SK", "SAS"},
    {"SY", "SCX"}, {"TK", "THY"}, {"TP", "TAP"}, {"UA", "UAL"}, {"VS", "VIR"},
    {"WN", "SWA"}, {"WS", "WJA"}, {"EI", "EIN"}, {"AY", "FIN"}, {"OS", "AUA"},
};

void setPhase(Phase p) {
  s_status.phase = p;
  const char* label = phaseLabel(p);
  strncpy(s_status.phase_label, label, sizeof(s_status.phase_label) - 1);
  s_status.phase_label[sizeof(s_status.phase_label) - 1] = '\0';
}

void formatLocalNow(char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  struct tm ti {};
  if (!getLocalTime(&ti, 20)) {
    return;
  }
  int hour12 = ti.tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  snprintf(out, out_len, "%d:%02d%s", hour12, ti.tm_min,
           (ti.tm_hour < 12) ? "a" : "p");
}

void formatLocalFromOffsetSec(int offset_sec, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  time_t now = 0;
  time(&now);
  if (now < 100000) {
    return;
  }
  now += offset_sec;
  struct tm eta {};
  localtime_r(&now, &eta);
  int hour12 = eta.tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  snprintf(out, out_len, "%d:%02d%s", hour12, eta.tm_min,
           (eta.tm_hour < 12) ? "a" : "p");
}

void noteBecameAirborne() {
  const bool from_ground = (s_status.phase == Phase::OnGround);
  s_ground_since_ms = 0;
  if (!s_status.have_takeoff) {
    formatLocalNow(s_status.takeoff_label, sizeof(s_status.takeoff_label));
    s_status.have_takeoff = (s_status.takeoff_label[0] != '\0');
    if (s_status.have_takeoff) {
      Serial.printf("flight_track: takeoff %s\n", s_status.takeoff_label);
    }
  }
  if (from_ground) {
    s_status.landing_label[0] = '\0';
    s_status.have_landing = false;
    s_post_landing_idle = false;
  }
  s_was_airborne = true;
  setPhase(Phase::Airborne);
}

void noteBecameOnGround() {
  if (s_ground_since_ms == 0) {
    s_ground_since_ms = millis();
  }
  if (s_was_airborne && !s_status.have_landing) {
    formatLocalNow(s_status.landing_label, sizeof(s_status.landing_label));
    s_status.have_landing = (s_status.landing_label[0] != '\0');
    s_status.eta_label[0] = '\0';
    if (s_status.have_landing) {
      Serial.printf("flight_track: landing %s — stopping live polls\n",
                    s_status.landing_label);
      s_post_landing_idle = true;
    }
  }
  setPhase(Phase::OnGround);
}

void updateEta() {
  s_status.eta_label[0] = '\0';
  if (s_status.have_landing || !s_status.has_dest || !s_status.has_position) {
    return;
  }
  if (s_status.gs_knots < 80.0f) {
    return;
  }
  const float dx =
      (s_status.dest_lon - s_status.lon) * kKmPerDeg *
      cosf(s_status.lat * 0.01745329252f);
  const float dy = (s_status.dest_lat - s_status.lat) * kKmPerDeg;
  const float dist_km = sqrtf(dx * dx + dy * dy);
  if (dist_km < 5.0f) {
    return;
  }
  const float km_per_hour = s_status.gs_knots * 1.852f;
  const float hours = dist_km / km_per_hour;
  const int sec = static_cast<int>(hours * 3600.0f + 0.5f);
  if (sec < 60 || sec > 36 * 3600) {
    return;
  }
  formatLocalFromOffsetSec(sec, s_status.eta_label, sizeof(s_status.eta_label));
}

void persistTrack() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  if (s_status.callsign[0] != '\0') {
    prefs.putString(kKeyCallsign, s_status.callsign);
    prefs.putString(kKeyOrigin, s_status.origin_iata);
    prefs.putString(kKeyDest, s_status.dest_iata);
  } else {
    prefs.remove(kKeyCallsign);
    prefs.remove(kKeyOrigin);
    prefs.remove(kKeyDest);
  }
  prefs.end();
}

void normalizeCallsign(const char* in, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (in == nullptr) {
    return;
  }
  size_t n = 0;
  for (const char* p = in; *p != '\0' && n + 1 < out_len; ++p) {
    char c = *p;
    if (c == ' ' || c == '-' || c == '_') {
      continue;
    }
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out[n++] = c;
    }
  }
  out[n] = '\0';
}

void normalizeAirportCode(const char* in, char* out, size_t out_len) {
  normalizeCallsign(in, out, out_len);
  // Keep at most 4 chars (ICAO); IATA is 3.
  if (out_len > 5) {
    out[4] = '\0';
  }
}

bool callsignsEqual(const char* a, const char* b) {
  char na[9];
  char nb[9];
  normalizeCallsign(a, na, sizeof(na));
  normalizeCallsign(b, nb, sizeof(nb));
  return na[0] != '\0' && strcmp(na, nb) == 0;
}

/** DL2460 → DAL2460 when the id looks like IATA airline + flight number. */
void expandAdsbCallsign(const char* display_cs, char* adsb_out, size_t out_len) {
  normalizeCallsign(display_cs, adsb_out, out_len);
  if (out_len < 5 || adsb_out[0] == '\0') {
    return;
  }
  const size_t len = strlen(adsb_out);
  // Already ICAO-style (three letters then a digit): keep as-is.
  if (len >= 4 && adsb_out[0] >= 'A' && adsb_out[0] <= 'Z' &&
      adsb_out[1] >= 'A' && adsb_out[1] <= 'Z' && adsb_out[2] >= 'A' &&
      adsb_out[2] <= 'Z' && adsb_out[3] >= '0' && adsb_out[3] <= '9') {
    return;
  }
  // IATA-style: two letters then a digit.
  if (len < 3 || !(adsb_out[0] >= 'A' && adsb_out[0] <= 'Z') ||
      !(adsb_out[1] >= 'A' && adsb_out[1] <= 'Z') ||
      !(adsb_out[2] >= '0' && adsb_out[2] <= '9')) {
    return;
  }
  for (const auto& row : kAirlineCodes) {
    if (adsb_out[0] == row.iata[0] && adsb_out[1] == row.iata[1]) {
      char expanded[12];
      snprintf(expanded, sizeof(expanded), "%s%s", row.icao, adsb_out + 2);
      strncpy(adsb_out, expanded, out_len - 1);
      adsb_out[out_len - 1] = '\0';
      return;
    }
  }
}

bool lookupAirportCoords(const char* code, float* lat, float* lon) {
  if (code == nullptr || code[0] == '\0' || lat == nullptr || lon == nullptr) {
    return false;
  }
  char icao[5] = "";
  const size_t n = strlen(code);
  if (n == 4) {
    strncpy(icao, code, sizeof(icao) - 1);
  } else if (n == 3) {
    // US large airports: IATA XYZ → ICAO KXYZ (also try as-is for odd cases).
    snprintf(icao, sizeof(icao), "K%s", code);
  } else {
    return false;
  }

  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    if (strcmp(data::large_airports::kAirports[i].ident, icao) == 0) {
      *lat = data::large_airports::kAirports[i].lat_e7 / 1e7f;
      *lon = data::large_airports::kAirports[i].lon_e7 / 1e7f;
      return true;
    }
  }
  return false;
}

void updateDistance(double center_lat, double center_lon) {
  if (!s_status.has_position) {
    s_status.dist_km = -1.0f;
    return;
  }
  const float dx =
      static_cast<float>(s_status.lon - center_lon) * kKmPerDeg;
  const float dy =
      static_cast<float>(s_status.lat - center_lat) * kKmPerDeg;
  s_status.dist_km = sqrtf(dx * dx + dy * dy);
}

void buildRouteLine() {
  s_status.route_line[0] = '\0';
  if (s_status.origin_iata[0] == '\0' && s_status.dest_iata[0] == '\0') {
    return;
  }
  const char* o = s_status.origin_iata[0] ? s_status.origin_iata : "???";
  const char* d = s_status.dest_iata[0] ? s_status.dest_iata : "???";
  snprintf(s_status.route_line, sizeof(s_status.route_line), "%s > %s", o, d);
}

void applyAirports(const char* origin, const char* dest) {
  char o[5] = "";
  char d[5] = "";
  normalizeAirportCode(origin, o, sizeof(o));
  normalizeAirportCode(dest, d, sizeof(d));

  if (o[0]) {
    // Prefer showing IATA (3-letter) on the UI when we have Kxxx.
    if (strlen(o) == 4 && o[0] == 'K') {
      strncpy(s_status.origin_iata, o + 1, sizeof(s_status.origin_iata) - 1);
    } else {
      strncpy(s_status.origin_iata, o, sizeof(s_status.origin_iata) - 1);
    }
    s_status.origin_iata[sizeof(s_status.origin_iata) - 1] = '\0';
  }
  if (d[0]) {
    if (strlen(d) == 4 && d[0] == 'K') {
      strncpy(s_status.dest_iata, d + 1, sizeof(s_status.dest_iata) - 1);
    } else {
      strncpy(s_status.dest_iata, d, sizeof(s_status.dest_iata) - 1);
    }
    s_status.dest_iata[sizeof(s_status.dest_iata) - 1] = '\0';
  }

  float lat = 0.0f;
  float lon = 0.0f;
  if (d[0] && lookupAirportCoords(d, &lat, &lon)) {
    s_status.dest_lat = lat;
    s_status.dest_lon = lon;
    s_status.has_dest = true;
  }
  buildRouteLine();
}

bool httpsGet(const char* url, String* payload) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(kRequestTimeoutMs);
  http.setConnectTimeout(200);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  int code = 0;
  while (millis() < deadline) {
    wifiLoop();
    code = http.GET();
    if (code > 0) {
      break;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      break;
    }
    delay(5);
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("flight_track: HTTP %d\n", code);
    http.end();
    return false;
  }
  *payload = http.getString();
  http.end();
  return payload->length() > 0;
}

bool planeOnGround(const JsonObject& plane) {
  return plane["alt_baro"].is<const char*>() &&
         strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void applyLiveFromPlane(const JsonObject& plane, bool on_ground) {
  if (plane["lat"].is<float>() && plane["lon"].is<float>()) {
    s_status.lat = plane["lat"].as<float>();
    s_status.lon = plane["lon"].as<float>();
    s_status.has_position = true;
  }

  if (plane["track"].is<float>() || plane["track"].is<int>()) {
    s_status.track_deg = plane["track"].as<float>();
  }
  if (plane["gs"].is<float>() || plane["gs"].is<int>()) {
    s_status.gs_knots = plane["gs"].as<float>();
  } else if (plane["tas"].is<float>()) {
    s_status.gs_knots = plane["tas"].as<float>();
  }

  if (plane["t"].is<const char*>()) {
    strncpy(s_status.type, plane["t"].as<const char*>(),
            sizeof(s_status.type) - 1);
    s_status.type[sizeof(s_status.type) - 1] = '\0';
  }

  s_status.alt[0] = '\0';
  if (on_ground) {
    strncpy(s_status.alt, "GND", sizeof(s_status.alt) - 1);
  } else if (plane["alt_baro"].is<float>() || plane["alt_baro"].is<int>()) {
    snprintf(s_status.alt, sizeof(s_status.alt), "%d ft",
             static_cast<int>(lroundf(plane["alt_baro"].as<float>())));
  } else if (plane["alt_geom"].is<float>() || plane["alt_geom"].is<int>()) {
    snprintf(s_status.alt, sizeof(s_status.alt), "%d ft",
             static_cast<int>(lroundf(plane["alt_geom"].as<float>())));
  } else if (planeOnGround(plane)) {
    strncpy(s_status.alt, "GND", sizeof(s_status.alt) - 1);
    on_ground = true;
  }

  s_status.last_seen_ms = millis();
  if (on_ground) {
    noteBecameOnGround();
  } else {
    noteBecameAirborne();
  }
}

bool matchesTracked(const char* flight) {
  if (flight == nullptr || flight[0] == '\0') {
    return false;
  }
  if (callsignsEqual(flight, s_status.callsign)) {
    return true;
  }
  if (s_status.adsb_callsign[0] != '\0' &&
      callsignsEqual(flight, s_status.adsb_callsign)) {
    return true;
  }
  return false;
}

bool matchLocalAircraft() {
  const size_t n = adsb::aircraftCount();
  const adsb::Aircraft* list = adsb::aircraftList();
  for (size_t i = 0; i < n; ++i) {
    if (!matchesTracked(list[i].callsign)) {
      continue;
    }
    s_status.lat = list[i].lat;
    s_status.lon = list[i].lon;
    s_status.has_position = true;
    s_status.track_deg = list[i].track_deg;
    s_status.gs_knots = list[i].gs_knots;
    strncpy(s_status.type, list[i].type, sizeof(s_status.type) - 1);
    s_status.type[sizeof(s_status.type) - 1] = '\0';
    strncpy(s_status.alt, list[i].alt, sizeof(s_status.alt) - 1);
    s_status.alt[sizeof(s_status.alt) - 1] = '\0';
    s_status.last_seen_ms = millis();
    const bool ground = (strcmp(list[i].alt, "GND") == 0);
    if (ground) {
      noteBecameOnGround();
    } else {
      noteBecameAirborne();
    }
    return true;
  }
  return false;
}

bool fetchLiveForId(const char* id) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  for (size_t i = 0; i < kLiveApiCount; ++i) {
    char url[112];
    snprintf(url, sizeof(url), "%s%s", kLiveCallsignApis[i], id);
    String payload;
    if (!httpsGet(url, &payload)) {
      if (i + 1 < kLiveApiCount) {
        delay(400);
      }
      continue;
    }
    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
      continue;
    }
    JsonArray ac = doc["ac"].as<JsonArray>();
    if (ac.isNull() || ac.size() == 0) {
      if (i + 1 < kLiveApiCount) {
        delay(400);
      }
      continue;
    }
    JsonObject plane = ac[0];
    applyLiveFromPlane(plane, planeOnGround(plane));
    Serial.printf("flight_track: live hit via api[%u] %s\n",
                  static_cast<unsigned>(i), id);
    return true;
  }
  return false;
}

bool fetchLiveByCallsign() {
  if (fetchLiveForId(s_status.adsb_callsign)) {
    return true;
  }
  if (s_status.adsb_callsign[0] != '\0' &&
      strcmp(s_status.adsb_callsign, s_status.callsign) != 0) {
    delay(1100);
    return fetchLiveForId(s_status.callsign);
  }
  return false;
}

void fetchAirlineMetaOnce() {
  if (s_status.route_ok) {
    return;
  }
  // Airline name + ICAO callsign only. Do NOT trust origin/destination —
  // adsbdb maps callsigns to a static schedule that is often wrong for today.
  char url[96];
  snprintf(url, sizeof(url), "%s%s", kRouteApi, s_status.callsign);
  String payload;
  if (!httpsGet(url, &payload)) {
    // Still allow live polling; mark ok so we don't hammer a dead endpoint.
    s_status.route_ok = true;
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    s_status.route_ok = true;
    return;
  }
  JsonObject route = doc["response"]["flightroute"];
  if (route.isNull()) {
    s_status.route_ok = true;
    return;
  }
  if (route["airline"]["name"].is<const char*>()) {
    strncpy(s_status.airline, route["airline"]["name"].as<const char*>(),
            sizeof(s_status.airline) - 1);
    s_status.airline[sizeof(s_status.airline) - 1] = '\0';
  }
  if (route["callsign_icao"].is<const char*>()) {
    char icao[9];
    normalizeCallsign(route["callsign_icao"].as<const char*>(), icao,
                      sizeof(icao));
    if (icao[0] != '\0') {
      strncpy(s_status.adsb_callsign, icao, sizeof(s_status.adsb_callsign) - 1);
      s_status.adsb_callsign[sizeof(s_status.adsb_callsign) - 1] = '\0';
      Serial.printf("flight_track: ADS-B id %s (display %s)\n",
                    s_status.adsb_callsign, s_status.callsign);
    }
  }
  s_status.route_ok = true;
  Serial.printf("flight_track: airline meta %s (%s)\n", s_status.callsign,
                s_status.airline[0] ? s_status.airline : "—");
}

void resetLiveFields() {
  s_status.has_position = false;
  s_status.lat = 0.0f;
  s_status.lon = 0.0f;
  s_status.track_deg = 0.0f;
  s_status.gs_knots = 0.0f;
  s_status.dist_km = -1.0f;
  s_status.type[0] = '\0';
  s_status.alt[0] = '\0';
  s_status.eta_label[0] = '\0';
}

void maybeAutoEnd() {
  if (!s_status.active) {
    return;
  }
  const unsigned long now = millis();
  if (s_status.phase == Phase::Searching &&
      (now - s_started_ms) >= config::kFlightTrackSearchMs) {
    Serial.println("flight_track: search timeout — clearing");
    clear();
    return;
  }
  if (s_was_airborne && s_status.phase == Phase::OnGround &&
      s_ground_since_ms != 0 &&
      (now - s_ground_since_ms) >= config::kFlightTrackLandedMs) {
    Serial.println("flight_track: landed hold done — clearing");
    clear();
    return;
  }
  if (s_was_airborne && s_status.last_seen_ms != 0 &&
      (now - s_status.last_seen_ms) >= config::kFlightTrackLostMs) {
    setPhase(Phase::Lost);
    if ((now - s_status.last_seen_ms) >=
        config::kFlightTrackLostMs + 5UL * 60UL * 1000UL) {
      Serial.println("flight_track: lost — clearing");
      clear();
    }
  }
}

}  // namespace

const char* phaseLabel(Phase phase) {
  switch (phase) {
    case Phase::Searching:
      return "Searching";
    case Phase::Airborne:
      return "Airborne";
    case Phase::OnGround:
      return "On ground";
    case Phase::Lost:
      return "Lost";
    case Phase::None:
    default:
      return "None";
  }
}

void init() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const String cs = prefs.getString(kKeyCallsign, "");
  const String orig = prefs.getString(kKeyOrigin, "");
  const String dest = prefs.getString(kKeyDest, "");
  prefs.end();
  if (cs.length() == 0) {
    return;
  }
  char norm[9];
  normalizeCallsign(cs.c_str(), norm, sizeof(norm));
  if (norm[0] == '\0') {
    return;
  }
  strncpy(s_status.callsign, norm, sizeof(s_status.callsign) - 1);
  s_status.callsign[sizeof(s_status.callsign) - 1] = '\0';
  expandAdsbCallsign(s_status.callsign, s_status.adsb_callsign,
                     sizeof(s_status.adsb_callsign));
  s_status.active = true;
  s_status.route_ok = false;
  s_started_ms = millis();
  s_was_airborne = false;
  s_ground_since_ms = 0;
  s_post_landing_idle = false;
  resetLiveFields();
  if (orig.length() || dest.length()) {
    applyAirports(orig.c_str(), dest.c_str());
  }
  setPhase(Phase::Searching);
  Serial.printf("flight_track: restored %s (adsb %s) %s\n", s_status.callsign,
                s_status.adsb_callsign, s_status.route_line);
}

const Status& status() { return s_status; }

bool isActive() { return s_status.active; }

const char* callsign() { return s_status.callsign; }

const char* adsbCallsign() {
  return s_status.adsb_callsign[0] ? s_status.adsb_callsign : s_status.callsign;
}

bool start(const char* flight_or_callsign, const char* origin,
           const char* dest) {
  char norm[9];
  normalizeCallsign(flight_or_callsign, norm, sizeof(norm));
  if (norm[0] == '\0' || strlen(norm) < 2) {
    return false;
  }
  clear();
  strncpy(s_status.callsign, norm, sizeof(s_status.callsign) - 1);
  s_status.callsign[sizeof(s_status.callsign) - 1] = '\0';
  expandAdsbCallsign(s_status.callsign, s_status.adsb_callsign,
                     sizeof(s_status.adsb_callsign));
  s_status.active = true;
  s_status.route_ok = false;
  s_status.airline[0] = '\0';
  s_status.origin_iata[0] = '\0';
  s_status.dest_iata[0] = '\0';
  s_status.route_line[0] = '\0';
  s_status.has_dest = false;
  s_started_ms = millis();
  s_last_poll_ms = 0;
  s_was_airborne = false;
  s_ground_since_ms = 0;
  s_post_landing_idle = false;
  resetLiveFields();
  setPhase(Phase::Searching);
  if ((origin && origin[0]) || (dest && dest[0])) {
    applyAirports(origin, dest);
  }
  persistTrack();
  Serial.printf("flight_track: tracking %s (adsb %s) %s\n", s_status.callsign,
                s_status.adsb_callsign,
                s_status.route_line[0] ? s_status.route_line : "(no route)");
  return true;
}

void clear() {
  s_status = Status{};
  setPhase(Phase::None);
  s_started_ms = 0;
  s_last_poll_ms = 0;
  s_ground_since_ms = 0;
  s_was_airborne = false;
  s_post_landing_idle = false;
  persistTrack();
  Serial.println("flight_track: cleared");
}

bool isPostLandingIdle() {
  return s_status.active && s_post_landing_idle;
}

void pollUpdate(double center_lat, double center_lon, bool allow_network) {
  if (!s_status.active || WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long now = millis();

  // Cheap: refresh from the local area ADS-B snapshot when present (no HTTP).
  if (!s_post_landing_idle && matchLocalAircraft()) {
    updateDistance(center_lat, center_lon);
    updateEta();
    maybeAutoEnd();
    return;
  }

  // Post-landing: keep final Out/In on screen; no more HTTP until auto-clear.
  if (s_post_landing_idle) {
    updateDistance(center_lat, center_lon);
    maybeAutoEnd();
    return;
  }

  if (!allow_network) {
    maybeAutoEnd();
    return;
  }

  const unsigned long interval =
      (!s_was_airborne && s_status.phase == Phase::Searching)
          ? config::kFlightTrackSearchPollMs
          : config::kFlightTrackPollMs;
  if (s_last_poll_ms != 0 && (now - s_last_poll_ms) < interval) {
    maybeAutoEnd();
    return;
  }
  s_last_poll_ms = now;

  if (!s_status.route_ok) {
    fetchAirlineMetaOnce();
    delay(1100);
  }

  const bool found = fetchLiveByCallsign();
  if (!found) {
    if (s_was_airborne && s_status.last_seen_ms != 0 &&
        (now - s_status.last_seen_ms) >= config::kFlightTrackLostMs) {
      setPhase(Phase::Lost);
    } else if (!s_was_airborne) {
      setPhase(Phase::Searching);
    }
  }

  updateDistance(center_lat, center_lon);
  updateEta();
  maybeAutoEnd();
}

}  // namespace services::flight_track
