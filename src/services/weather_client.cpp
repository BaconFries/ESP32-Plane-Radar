#include "services/weather_client.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "services/time_sync.h"

namespace services::weather {

namespace {

Conditions s_current{};

/** Parse Open-Meteo ISO local time "2026-08-10T06:42" → minutes from midnight. */
bool parseIsoLocalToMinutes(const char* iso, int* out_min) {
  if (iso == nullptr || out_min == nullptr) {
    return false;
  }
  int hour = 0;
  int minute = 0;
  // Accept "...THH:MM" or "...THH:MM:SS"
  const char* t = strchr(iso, 'T');
  if (t == nullptr) {
    return false;
  }
  if (sscanf(t + 1, "%d:%d", &hour, &minute) < 2) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return false;
  }
  *out_min = hour * 60 + minute;
  return true;
}

}  // namespace

const char* summaryForCode(int weather_code) {
  if (weather_code == 0) {
    return "Clear";
  }
  if (weather_code >= 1 && weather_code <= 3) {
    return "Cloudy";
  }
  if (weather_code == 45 || weather_code == 48) {
    return "Fog";
  }
  if (weather_code >= 51 && weather_code <= 67) {
    return "Rain";
  }
  if (weather_code >= 71 && weather_code <= 77) {
    return "Snow";
  }
  if (weather_code >= 80 && weather_code <= 82) {
    return "Showers";
  }
  if (weather_code == 85 || weather_code == 86) {
    return "Snow";
  }
  if (weather_code >= 95 && weather_code <= 99) {
    return "Storm";
  }
  return "Weather";
}

const Conditions& current() { return s_current; }

bool needsRefresh(unsigned long interval_ms) {
  if (!s_current.valid) {
    return true;
  }
  return (millis() - s_current.fetched_ms) >= interval_ms;
}

bool solarTimes(int& sunrise_min, int& sunset_min) {
  if (s_current.sunrise_min < 0 || s_current.sunset_min < 0) {
    return false;
  }
  sunrise_min = s_current.sunrise_min;
  sunset_min = s_current.sunset_min;
  return true;
}

bool fetchUpdate(double lat, double lon) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  char url[256];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code"
           "&daily=sunrise,sunset&forecast_days=1&timezone=auto",
           lat, lon);

  HTTPClient http;
  if (!http.begin(url)) {
    Serial.println("weather: begin failed");
    return false;
  }
  http.setTimeout(8000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("weather: HTTP %d\n", code);
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("weather: JSON parse error");
    return false;
  }
  if (doc["error"].is<bool>() && doc["error"].as<bool>()) {
    Serial.println("weather: API error");
    return false;
  }

  JsonObject cur = doc["current"];
  if (cur.isNull() || !cur["temperature_2m"].is<float>()) {
    Serial.println("weather: missing current");
    return false;
  }

  s_current.temp_c = cur["temperature_2m"].as<float>();
  s_current.weather_code = cur["weather_code"] | 0;
  strncpy(s_current.summary, summaryForCode(s_current.weather_code),
          sizeof(s_current.summary) - 1);
  s_current.summary[sizeof(s_current.summary) - 1] = '\0';
  s_current.utc_offset_sec = doc["utc_offset_seconds"] | 0;
  s_current.fetched_ms = millis();
  s_current.valid = true;

  s_current.sunrise_min = -1;
  s_current.sunset_min = -1;
  JsonObject daily = doc["daily"];
  if (!daily.isNull()) {
    JsonArray sunrise_arr = daily["sunrise"].as<JsonArray>();
    JsonArray sunset_arr = daily["sunset"].as<JsonArray>();
    if (!sunrise_arr.isNull() && !sunset_arr.isNull() &&
        sunrise_arr.size() > 0 && sunset_arr.size() > 0) {
      const char* sr = sunrise_arr[0].as<const char*>();
      const char* ss = sunset_arr[0].as<const char*>();
      int srm = -1;
      int ssm = -1;
      if (parseIsoLocalToMinutes(sr, &srm) &&
          parseIsoLocalToMinutes(ss, &ssm)) {
        s_current.sunrise_min = srm;
        s_current.sunset_min = ssm;
      }
    }
  }

  time_sync::setUtcOffsetSeconds(s_current.utc_offset_sec);

  Serial.printf("weather: %.1f C, %s (code %d)", s_current.temp_c,
                s_current.summary, s_current.weather_code);
  if (s_current.sunrise_min >= 0) {
    Serial.printf(", sun %02d:%02d–%02d:%02d", s_current.sunrise_min / 60,
                  s_current.sunrise_min % 60, s_current.sunset_min / 60,
                  s_current.sunset_min % 60);
  }
  Serial.println();
  return true;
}

}  // namespace services::weather
