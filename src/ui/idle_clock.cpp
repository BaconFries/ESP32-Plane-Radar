#include "ui/idle_clock.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cstdio>
#include <cmath>
#include <ctime>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/time_sync.h"
#include "services/weather_client.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui {
namespace {

int s_drawn_minute = -1;
int s_drawn_day = -1;
bool s_drawn_weather_valid = false;
float s_drawn_temp_c = -999.0f;

/** Bright CRT-style green for the clock digits. */
constexpr uint8_t kClockGreenR = 40;
constexpr uint8_t kClockGreenG = 255;
constexpr uint8_t kClockGreenB = 90;

void formatTemp(char* buf, size_t len, float temp_c) {
  if (radar::useMiles()) {
    const int f = static_cast<int>(lroundf(temp_c * 9.0f / 5.0f + 32.0f));
    snprintf(buf, len, "%dF", f);
  } else {
    const int c = static_cast<int>(lroundf(temp_c));
    snprintf(buf, len, "%dC", c);
  }
}

/** Bitmap fonts — prefer size 1; callers may setTextSize for modest scale. */
void useBitmap(const lgfx::GFXfont* font, uint8_t size = 1) {
  displayFontSetBitmap(tft, font);
  tft.setTextSize(size);
}

void drawScreen(const struct tm& ti) {
  const uint16_t bg = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  const uint16_t clock_green =
      tft.color565(kClockGreenR, kClockGreenG, kClockGreenB);
  const uint16_t date_fg = tft.color565(180, 230, 190);
  const uint16_t weather_fg = tft.color565(230, 245, 255);

  tft.fillScreen(bg);
  tft.setTextDatum(textdatum_t::middle_center);

  int hour12 = ti.tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  const char* ampm = (ti.tm_hour < 12) ? "AM" : "PM";

  char date_buf[24];
  static const char* kDow[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  snprintf(date_buf, sizeof(date_buf), "%s %s %d", kDow[ti.tm_wday % 7],
           kMon[ti.tm_mon % 12], ti.tm_mday);

  // Date at the top so the time can sit in the wide center.
  useBitmap(&fonts::FreeSansBold12pt7b, 1);
  tft.setTextColor(date_fg, bg);
  tft.drawString(date_buf, radar::kCenterX, radar::kCenterY - 72);

  char time_buf[8];
  snprintf(time_buf, sizeof(time_buf), "%d:%02d", hour12, ti.tm_min);

  // Slightly larger than before now that date is out of the way.
  useBitmap(&fonts::FreeSansBold24pt7b, 1);
  tft.setTextSize(1.75f);
  tft.setTextColor(clock_green, bg);
  tft.drawString(time_buf, radar::kCenterX, radar::kCenterY - 12);

  useBitmap(&fonts::FreeSansBold18pt7b, 1);
  tft.setTextColor(clock_green, bg);
  tft.drawString(ampm, radar::kCenterX, radar::kCenterY + 34);

  const auto& wx = services::weather::current();
  if (wx.valid) {
    char line[32];
    char temp[12];
    formatTemp(temp, sizeof(temp), wx.temp_c);
    snprintf(line, sizeof(line), "%s  %s", temp, wx.summary);
    useBitmap(&fonts::FreeSansBold12pt7b, 1);
    tft.setTextColor(weather_fg, bg);
    tft.drawString(line, radar::kCenterX, radar::kCenterY + 68);
  } else {
    useBitmap(&fonts::FreeSansBold12pt7b, 1);
    tft.setTextColor(date_fg, bg);
    tft.drawString("Weather…", radar::kCenterX, radar::kCenterY + 68);
  }

  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextSize(1);
  s_drawn_minute = ti.tm_hour * 60 + ti.tm_min;
  s_drawn_day = ti.tm_yday;
  s_drawn_weather_valid = wx.valid;
  s_drawn_temp_c = wx.temp_c;
}

}  // namespace

void idleClockDraw() {
  struct tm ti {};
  if (!getLocalTime(&ti, 50)) {
    const uint16_t bg = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
    tft.fillScreen(bg);
    tft.setTextColor(tft.color565(kClockGreenR, kClockGreenG, kClockGreenB), bg);
    tft.setTextDatum(textdatum_t::middle_center);
    displayFontSetBitmap(tft, &fonts::FreeSansBold18pt7b);
    tft.drawString("Syncing time…", radar::kCenterX, radar::kCenterY);
    tft.setTextDatum(textdatum_t::top_left);
    s_drawn_minute = -1;
    return;
  }
  drawScreen(ti);
}

bool idleClockTick() {
  struct tm ti {};
  if (!getLocalTime(&ti, 0)) {
    return false;
  }
  const int minute = ti.tm_hour * 60 + ti.tm_min;
  const auto& wx = services::weather::current();
  const bool weather_changed =
      (wx.valid != s_drawn_weather_valid) ||
      (wx.valid && fabsf(wx.temp_c - s_drawn_temp_c) > 0.4f);
  if (minute == s_drawn_minute && ti.tm_yday == s_drawn_day && !weather_changed) {
    return false;
  }
  drawScreen(ti);
  return true;
}

}  // namespace ui
