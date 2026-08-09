#include "ui/tracked_flight.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cstdio>
#include <cmath>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/flight_track.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui {
namespace {

char s_drawn_cs[9] = "";
char s_drawn_alt[12] = "";
char s_drawn_phase[16] = "";
char s_drawn_times[24] = "";
char s_drawn_type[5] = "";
int s_drawn_gs = -1;

constexpr uint8_t kGreenR = 40;
constexpr uint8_t kGreenG = 255;
constexpr uint8_t kGreenB = 90;

void useBitmap(const lgfx::GFXfont* font, float size = 1.0f) {
  displayFontSetBitmap(tft, font);
  tft.setTextSize(size);
}

void drawLine(const char* text, int y, uint16_t fg, uint16_t bg,
              const lgfx::GFXfont* gfx, float size = 1.0f) {
  tft.setTextColor(fg, bg);
  useBitmap(gfx, size);
  tft.drawString(text, radar::kCenterX, y);
}

void formatTimesLine(const services::flight_track::Status& st, char* out,
                     size_t out_len) {
  out[0] = '\0';
  const char* out_t = st.have_takeoff ? st.takeoff_label : "—";
  if (st.have_landing) {
    snprintf(out, out_len, "Out %s  In %s", out_t, st.landing_label);
  } else if (st.eta_label[0] != '\0') {
    snprintf(out, out_len, "Out %s  ETA %s", out_t, st.eta_label);
  } else {
    snprintf(out, out_len, "Out %s  In —", out_t);
  }
}

void formatStatusLine(const services::flight_track::Status& st, char* out,
                      size_t out_len) {
  const char* phase = st.phase_label[0] ? st.phase_label : "—";
  if (st.type[0] != '\0') {
    snprintf(out, out_len, "%s  %s", phase, st.type);
  } else {
    snprintf(out, out_len, "%s", phase);
  }
}

void drawScreen() {
  const auto& st = services::flight_track::status();
  const uint16_t bg = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t muted = tft.color565(160, 185, 200);
  const uint16_t accent = tft.color565(255, 200, 80);
  const uint16_t ok = tft.color565(80, 220, 140);
  const uint16_t green = tft.color565(kGreenR, kGreenG, kGreenB);

  tft.fillScreen(bg);
  tft.setTextDatum(textdatum_t::middle_center);

  if (!st.active) {
    drawLine("No flight", radar::kCenterY - 18, fg, bg,
             &fonts::FreeSansBold18pt7b);
    drawLine("tracked", radar::kCenterY + 22, muted, bg,
             &fonts::FreeSansBold18pt7b);
    tft.setTextDatum(textdatum_t::top_left);
    tft.setTextSize(1);
    s_drawn_cs[0] = '\0';
    s_drawn_alt[0] = '\0';
    s_drawn_phase[0] = '\0';
    s_drawn_times[0] = '\0';
    s_drawn_type[0] = '\0';
    s_drawn_gs = -1;
    return;
  }

  // Compact callsign near the top.
  const char* cs = st.callsign[0] ? st.callsign : "—";
  drawLine(cs, radar::kCenterY - 78, green, bg, &fonts::FreeSansBold18pt7b);

  if (st.route_line[0] != '\0') {
    drawLine(st.route_line, radar::kCenterY - 48, accent, bg,
             &fonts::FreeSansBold12pt7b);
  }

  // Out / ETA in the wide middle band (9pt — closest available to ~10pt).
  char times[28];
  formatTimesLine(st, times, sizeof(times));
  drawLine(times, radar::kCenterY - 8, fg, bg, &fonts::FreeSansBold9pt7b);

  uint16_t phase_color = muted;
  if (st.phase == services::flight_track::Phase::Airborne) {
    phase_color = ok;
  } else if (st.phase == services::flight_track::Phase::Lost) {
    phase_color = tft.color565(255, 90, 90);
  } else if (st.phase == services::flight_track::Phase::OnGround) {
    phase_color = accent;
  }

  char status_line[28];
  formatStatusLine(st, status_line, sizeof(status_line));
  drawLine(status_line, radar::kCenterY + 28, phase_color, bg,
           &fonts::FreeSansBold12pt7b);

  char detail[40];
  if (st.alt[0] != '\0' || st.gs_knots > 0.5f) {
    snprintf(detail, sizeof(detail), "%s  %.0f kt",
             st.alt[0] ? st.alt : "—", static_cast<double>(st.gs_knots));
    drawLine(detail, radar::kCenterY + 56, muted, bg, &fonts::FreeSansBold12pt7b);
  }

  if (st.dist_km >= 0.0f) {
    if (radar::useMiles()) {
      snprintf(detail, sizeof(detail), "%.0f mi away",
               static_cast<double>(st.dist_km / 1.609344f));
    } else {
      snprintf(detail, sizeof(detail), "%.0f km away",
               static_cast<double>(st.dist_km));
    }
    drawLine(detail, radar::kCenterY + 82, muted, bg, &fonts::FreeSansBold12pt7b);
  }

  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextSize(1);
  strncpy(s_drawn_cs, st.callsign, sizeof(s_drawn_cs) - 1);
  s_drawn_cs[sizeof(s_drawn_cs) - 1] = '\0';
  strncpy(s_drawn_alt, st.alt, sizeof(s_drawn_alt) - 1);
  s_drawn_alt[sizeof(s_drawn_alt) - 1] = '\0';
  strncpy(s_drawn_phase, st.phase_label, sizeof(s_drawn_phase) - 1);
  s_drawn_phase[sizeof(s_drawn_phase) - 1] = '\0';
  strncpy(s_drawn_times, times, sizeof(s_drawn_times) - 1);
  s_drawn_times[sizeof(s_drawn_times) - 1] = '\0';
  strncpy(s_drawn_type, st.type, sizeof(s_drawn_type) - 1);
  s_drawn_type[sizeof(s_drawn_type) - 1] = '\0';
  s_drawn_gs = static_cast<int>(lroundf(st.gs_knots));
}

}  // namespace

void trackedFlightDraw() {
  drawScreen();
}

bool trackedFlightTick() {
  const auto& st = services::flight_track::status();
  if (!st.active) {
    if (s_drawn_cs[0] == '\0' && s_drawn_phase[0] == '\0') {
      return false;
    }
    drawScreen();
    return true;
  }
  char times[28];
  formatTimesLine(st, times, sizeof(times));
  const int gs = static_cast<int>(lroundf(st.gs_knots));
  if (strcmp(s_drawn_cs, st.callsign) == 0 &&
      strcmp(s_drawn_alt, st.alt) == 0 &&
      strcmp(s_drawn_phase, st.phase_label) == 0 &&
      strcmp(s_drawn_times, times) == 0 &&
      strcmp(s_drawn_type, st.type) == 0 && gs == s_drawn_gs) {
    return false;
  }
  drawScreen();
  return true;
}

}  // namespace ui
