#include "ui/night_dim.h"

#include <ctime>

#include "config.h"
#include "hardware/display.h"
#include "services/weather_client.h"

namespace ui {
namespace {

unsigned long s_last_check_ms = 0;
bool s_force = true;
bool s_night = false;
bool s_known = false;
/** When true, sun/hour schedule will not override s_night. */
bool s_manual_override = false;

bool computeIsNight() {
  struct tm ti {};
  if (!getLocalTime(&ti, 0)) {
    return false;
  }

  const int now_min = ti.tm_hour * 60 + ti.tm_min;
  int sunrise = 0;
  int sunset = 0;
  if (services::weather::solarTimes(sunrise, sunset) && sunset > sunrise) {
    return now_min >= sunset || now_min < sunrise;
  }

  const int h = ti.tm_hour;
  if (config::kNightStartHour > config::kNightEndHour) {
    return h >= config::kNightStartHour || h < config::kNightEndHour;
  }
  return h >= config::kNightStartHour && h < config::kNightEndHour;
}

uint8_t scaleCh(uint8_t c) {
  return static_cast<uint8_t>((static_cast<uint16_t>(c) * config::kNightFgScale) /
                              255);
}

}  // namespace

bool nightDimActive() { return s_known && s_night; }

uint16_t themeColor565(uint8_t r, uint8_t g, uint8_t b, bool scale_fg) {
  if (scale_fg && nightDimActive()) {
    r = scaleCh(r);
    g = scaleCh(g);
    b = scaleCh(b);
  }
  return tft.color565(r, g, b);
}

void nightDimInvalidate() {
  s_force = true;
  s_last_check_ms = 0;
}

bool nightDimToggleForce() {
  s_manual_override = true;
  s_night = !s_night;
  s_known = true;
  Serial.printf("display: night theme %s (manual)\n", s_night ? "on" : "off");
  return true;
}

bool nightDimTick() {
  if (s_manual_override) {
    return false;
  }

  const unsigned long now = millis();
  if (!s_force && s_last_check_ms != 0 &&
      (now - s_last_check_ms) < config::kNightDimCheckMs) {
    return false;
  }
  s_last_check_ms = now;
  s_force = false;

  const bool night = computeIsNight();
  const bool changed = !s_known || (night != s_night);
  if (changed) {
    s_night = night;
    s_known = true;
    Serial.printf("display: night theme %s\n", night ? "on" : "off");
  }
  return changed;
}

}  // namespace ui
