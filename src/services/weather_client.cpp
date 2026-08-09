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

bool fetchUpdate(double lat, double lon) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  char url[192];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code&timezone=auto",
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

  time_sync::setUtcOffsetSeconds(s_current.utc_offset_sec);

  Serial.printf("weather: %.1f C, %s (code %d)\n", s_current.temp_c,
                s_current.summary, s_current.weather_code);
  return true;
}

}  // namespace services::weather
