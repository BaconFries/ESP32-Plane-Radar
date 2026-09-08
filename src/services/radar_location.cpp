#include "services/radar_location.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";
constexpr char kKeyZip[] = "zip";
constexpr size_t kZipMaxLen = 10;

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
char s_zip[kZipMaxLen + 1] = "";

bool parseCoord(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

void normalizeZip(const char* zip_str, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (zip_str == nullptr) {
    return;
  }
  size_t n = 0;
  for (const char* p = zip_str; *p != '\0' && n + 1 < out_len; ++p) {
    if (*p >= '0' && *p <= '9') {
      out[n++] = *p;
      if (n == 5) {
        break;
      }
    }
  }
  out[n] = '\0';
}

/** Resolve US ZIP via Zippopotam.us when STA Wi‑Fi has internet. */
bool lookupUsZip(const char* zip5, double* out_lat, double* out_lon) {
  if (zip5 == nullptr || strlen(zip5) != 5 || out_lat == nullptr ||
      out_lon == nullptr) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  char url[48];
  snprintf(url, sizeof(url), "https://api.zippopotam.us/us/%s", zip5);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("ZIP lookup: begin failed");
    return false;
  }
  http.setTimeout(8000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("ZIP lookup: HTTP %d\n", code);
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("ZIP lookup: JSON parse error");
    return false;
  }
  JsonObject place = doc["places"][0];
  if (place.isNull()) {
    Serial.println("ZIP lookup: no places");
    return false;
  }
  // API returns latitude/longitude as strings.
  const char* lat_s = place["latitude"];
  const char* lon_s = place["longitude"];
  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_s, &lat) || !parseCoord(lon_s, &lon) ||
      !validLatLon(lat, lon)) {
    Serial.println("ZIP lookup: bad coordinates");
    return false;
  }
  *out_lat = lat;
  *out_lon = lon;
  return true;
}

void persist(double lat, double lon, const char* zip) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat, lat);
  prefs.putDouble(kKeyLon, lon);
  if (zip != nullptr && zip[0] != '\0') {
    prefs.putString(kKeyZip, zip);
  } else {
    prefs.remove(kKeyZip);
  }
  prefs.end();
  s_lat = lat;
  s_lon = lon;
  if (zip != nullptr && zip[0] != '\0') {
    strncpy(s_zip, zip, kZipMaxLen);
    s_zip[kZipMaxLen] = '\0';
  } else {
    s_zip[0] = '\0';
  }
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
    }
  }
  if (prefs.isKey(kKeyZip)) {
    const String z = prefs.getString(kKeyZip, "");
    normalizeZip(z.c_str(), s_zip, sizeof(s_zip));
  }
  prefs.end();
  Serial.printf("Radar center: %.5f, %.5f", s_lat, s_lon);
  if (s_zip[0] != '\0') {
    Serial.printf(" (ZIP %s)", s_zip);
  }
  Serial.println();
}

double lat() { return s_lat; }

double lon() { return s_lon; }

const char* zip() { return s_zip; }

bool saveFromPortal(const char* lat_str, const char* lon_str, const char* zip_str) {
  char zip_norm[kZipMaxLen + 1];
  normalizeZip(zip_str, zip_norm, sizeof(zip_norm));

  double lat = 0.0;
  double lon = 0.0;
  bool have_coords = false;

  // Prefer a 5-digit ZIP over whatever lat/lon the form still shows (defaults).
  if (strlen(zip_norm) == 5) {
    if (lookupUsZip(zip_norm, &lat, &lon)) {
      have_coords = true;
      Serial.printf("ZIP %s resolved on device to %.6f, %.6f\n", zip_norm, lat,
                    lon);
    }
  }

  if (!have_coords) {
    if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon) ||
        !validLatLon(lat, lon)) {
      return false;
    }
  }

  persist(lat, lon, zip_norm);
  if (zip_norm[0] != '\0') {
    Serial.printf("Radar location saved: %.6f, %.6f (ZIP %s)\n", lat, lon,
                  zip_norm);
  } else {
    Serial.printf("Radar location saved: %.6f, %.6f\n", lat, lon);
  }
  return true;
}

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  return saveFromPortal(lat_str, lon_str, "");
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.remove(kKeyZip);
  prefs.end();
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  s_zip[0] = '\0';
}

}  // namespace services::location
