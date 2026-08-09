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
#include "services/adsb_client.h"
#include "services/wifi_setup.h"

namespace services::flight_track {

namespace {

constexpr char kPrefsNamespace[] = "flighttrk";
constexpr char kKeyCallsign[] = "cs";
constexpr char kCallsignApi[] = "https://opendata.adsb.fi/api/v2/callsign/";
constexpr char kRouteApi[] = "https://api.adsbdb.com/v0/callsign/";
constexpr float kKmPerDeg = 111.0f;
constexpr unsigned long kRequestTimeoutMs = 10000;

Status s_status{};
unsigned long s_started_ms = 0;
unsigned long s_last_poll_ms = 0;
unsigned long s_ground_since_ms = 0;
bool s_was_airborne = false;

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
      Serial.printf("flight_track: landing %s\n", s_status.landing_label);
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

void persistCallsign(const char* cs) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  if (cs != nullptr && cs[0] != '\0') {
    prefs.putString(kKeyCallsign, cs);
  } else {
    prefs.remove(kKeyCallsign);
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

bool callsignsEqual(const char* a, const char* b) {
  char na[9];
  char nb[9];
  normalizeCallsign(a, na, sizeof(na));
  normalizeCallsign(b, nb, sizeof(nb));
  return na[0] != '\0' && strcmp(na, nb) == 0;
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
  char url[96];
  snprintf(url, sizeof(url), "%s%s", kCallsignApi, id);
  String payload;
  if (!httpsGet(url, &payload)) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    return false;
  }
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull() || ac.size() == 0) {
    return false;
  }
  JsonObject plane = ac[0];
  applyLiveFromPlane(plane, planeOnGround(plane));
  return true;
}

bool fetchLiveByCallsign() {
  if (fetchLiveForId(s_status.adsb_callsign)) {
    return true;
  }
  if (s_status.adsb_callsign[0] != '\0' &&
      strcmp(s_status.adsb_callsign, s_status.callsign) != 0) {
    delay(1100);
  }
  if (strcmp(s_status.adsb_callsign, s_status.callsign) != 0) {
    return fetchLiveForId(s_status.callsign);
  }
  return false;
}

void fetchRouteOnce() {
  if (s_status.route_ok) {
    return;
  }
  // Prefer looking up with the user-entered id (IATA flight # often works on adsbdb).
  char url[96];
  snprintf(url, sizeof(url), "%s%s", kRouteApi, s_status.callsign);
  String payload;
  if (!httpsGet(url, &payload)) {
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    return;
  }
  JsonObject route = doc["response"]["flightroute"];
  if (route.isNull()) {
    return;
  }
  if (route["airline"]["name"].is<const char*>()) {
    strncpy(s_status.airline, route["airline"]["name"].as<const char*>(),
            sizeof(s_status.airline) - 1);
    s_status.airline[sizeof(s_status.airline) - 1] = '\0';
  }
  if (route["origin"]["iata_code"].is<const char*>()) {
    strncpy(s_status.origin_iata, route["origin"]["iata_code"].as<const char*>(),
            sizeof(s_status.origin_iata) - 1);
    s_status.origin_iata[sizeof(s_status.origin_iata) - 1] = '\0';
  }
  if (route["destination"]["iata_code"].is<const char*>()) {
    strncpy(s_status.dest_iata,
            route["destination"]["iata_code"].as<const char*>(),
            sizeof(s_status.dest_iata) - 1);
    s_status.dest_iata[sizeof(s_status.dest_iata) - 1] = '\0';
  }
  if (route["destination"]["latitude"].is<float>() &&
      route["destination"]["longitude"].is<float>()) {
    s_status.dest_lat = route["destination"]["latitude"].as<float>();
    s_status.dest_lon = route["destination"]["longitude"].as<float>();
    s_status.has_dest = true;
  }
  // ADS-B uses ICAO callsigns (DAL2460); keep the user's DL2460 for display.
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
  buildRouteLine();
  Serial.printf("flight_track: route %s %s\n", s_status.callsign,
                s_status.route_line);
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
    Serial.println("flight_track: landed — clearing");
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
  strncpy(s_status.adsb_callsign, norm, sizeof(s_status.adsb_callsign) - 1);
  s_status.adsb_callsign[sizeof(s_status.adsb_callsign) - 1] = '\0';
  s_status.active = true;
  s_started_ms = millis();
  s_was_airborne = false;
  s_ground_since_ms = 0;
  resetLiveFields();
  setPhase(Phase::Searching);
  Serial.printf("flight_track: restored %s\n", s_status.callsign);
}

const Status& status() { return s_status; }

bool isActive() { return s_status.active; }

const char* callsign() { return s_status.callsign; }

const char* adsbCallsign() {
  return s_status.adsb_callsign[0] ? s_status.adsb_callsign : s_status.callsign;
}

bool start(const char* flight_or_callsign) {
  char norm[9];
  normalizeCallsign(flight_or_callsign, norm, sizeof(norm));
  if (norm[0] == '\0' || strlen(norm) < 2) {
    return false;
  }
  clear();
  strncpy(s_status.callsign, norm, sizeof(s_status.callsign) - 1);
  s_status.callsign[sizeof(s_status.callsign) - 1] = '\0';
  strncpy(s_status.adsb_callsign, norm, sizeof(s_status.adsb_callsign) - 1);
  s_status.adsb_callsign[sizeof(s_status.adsb_callsign) - 1] = '\0';
  s_status.active = true;
  s_status.route_ok = false;
  s_status.airline[0] = '\0';
  s_status.origin_iata[0] = '\0';
  s_status.dest_iata[0] = '\0';
  s_status.route_line[0] = '\0';
  s_started_ms = millis();
  s_last_poll_ms = 0;
  s_was_airborne = false;
  s_ground_since_ms = 0;
  resetLiveFields();
  setPhase(Phase::Searching);
  persistCallsign(s_status.callsign);
  Serial.printf("flight_track: tracking %s\n", s_status.callsign);
  if (WiFi.status() == WL_CONNECTED) {
    fetchRouteOnce();
  }
  return true;
}

void clear() {
  s_status = Status{};
  setPhase(Phase::None);
  s_started_ms = 0;
  s_last_poll_ms = 0;
  s_ground_since_ms = 0;
  s_was_airborne = false;
  persistCallsign("");
  Serial.println("flight_track: cleared");
}

void pollUpdate(double center_lat, double center_lon) {
  if (!s_status.active || WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long now = millis();
  if (s_last_poll_ms != 0 &&
      (now - s_last_poll_ms) < config::kFlightTrackPollMs) {
    maybeAutoEnd();
    return;
  }
  s_last_poll_ms = now;

  if (!s_status.route_ok) {
    fetchRouteOnce();
    delay(1100);  // stay under adsb.fi 1 req/s when followed by callsign GET
  }

  bool found = matchLocalAircraft();
  if (!found) {
    found = fetchLiveByCallsign();
  }

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
