#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "services/wifi_setup.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
/** TLS handshake often exceeds a few hundred ms on ESP32-C3. */
constexpr int kConnectTimeoutMs = 8000;
constexpr unsigned long kRequestTimeoutMs = 12000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

void pollLight() {
  // Keep the BOOT long-press responsive, but do NOT run WiFiManager's web
  // server while this TLS socket is active — process() can corrupt the read.
  bootButtonPollLongPress();
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
  yield();
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

bool hasLatLon(const JsonObject& plane) {
  float lat = 0.0f;
  float lon = 0.0f;
  return readJsonFloat(plane, "lat", &lat) && readJsonFloat(plane, "lon", &lon);
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

/** Keep only fields we need (ArduinoJson filter docs: list keys under ac[0]). */
void buildAcFilter(JsonDocument& filter) {
  filter["total"] = true;
  filter["msg"] = true;
  filter["ac"][0]["lat"] = true;
  filter["ac"][0]["lon"] = true;
  filter["ac"][0]["flight"] = true;
  filter["ac"][0]["hex"] = true;
  filter["ac"][0]["t"] = true;
  filter["ac"][0]["alt_baro"] = true;
  filter["ac"][0]["alt_geom"] = true;
  filter["ac"][0]["gs"] = true;
  filter["ac"][0]["tas"] = true;
  filter["ac"][0]["ias"] = true;
  filter["ac"][0]["track"] = true;
  filter["ac"][0]["true_heading"] = true;
  filter["ac"][0]["mag_heading"] = true;
  filter["ac"][0]["dir"] = true;
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  char url[128];
  snprintf(url, sizeof(url), "%s%.5f/lon/%.5f/dist/%.1f", kApiBase, center_lat,
           center_lon, dist_nm);

  Serial.printf("adsb: query %.5f,%.5f  %.1f nm  (heap %u)\n", center_lat,
                center_lon, dist_nm, static_cast<unsigned>(ESP.getFreeHeap()));

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(kRequestTimeoutMs / 1000);

  HTTPClient http;
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");

  pollLight();
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  // Buffer the body first. Streaming + Filter on WiFiClientSecure was returning
  // Ok with an empty ac[] even when the API had traffic.
  String payload = http.getString();
  http.end();
  if (payload.length() == 0) {
    Serial.println("adsb: empty response");
    return false;
  }

  JsonDocument filter;
  buildAcFilter(filter);

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("adsb: JSON parse error: %s (bytes %u, head \"%.40s\")\n",
                  err.c_str(), static_cast<unsigned>(payload.length()),
                  payload.c_str());
    return false;
  }

  const int api_total = doc["total"] | -1;
  const char* api_msg = doc["msg"] | "";
  JsonArray ac = doc["ac"].as<JsonArray>();
  const size_t api_ac = ac.isNull() ? 0 : ac.size();

  if (api_ac == 0) {
    s_aircraft_count = 0;
    Serial.printf("adsb: 0 aircraft (api total=%d msg=\"%s\" bytes=%u)\n",
                  api_total, api_msg, static_cast<unsigned>(payload.length()));
    return true;
  }

  size_t n = 0;
  size_t skipped_ground = 0;
  size_t skipped_nopos = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!hasLatLon(plane)) {
      ++skipped_nopos;
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      ++skipped_ground;
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft (api %u, skip ground=%u nopos=%u)\n",
                static_cast<unsigned>(n), static_cast<unsigned>(api_ac),
                static_cast<unsigned>(skipped_ground),
                static_cast<unsigned>(skipped_nopos));
  return true;
}

}  // namespace services::adsb
