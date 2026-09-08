/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdint>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/flight_track.h"
#include "services/radar_location.h"
#include "services/time_sync.h"
#include "services/weather_client.h"
#include "services/wifi_setup.h"
#include "ui/idle_clock.h"
#include "ui/night_dim.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"
#include "ui/tracked_flight.h"

namespace {

enum class ManualScreen : uint8_t {
  Auto = 0,
  Clock,
  Track,
};

bool g_radar_visible = false;
bool g_idle_visible = false;
bool g_track_visible = false;
ManualScreen g_manual = ManualScreen::Auto;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_manual_poll_ms = 0;
unsigned long g_pending_tap_ms = 0;
uint8_t g_pending_tap_count = 0;
unsigned long g_auto_flip_ms = 0;
bool g_auto_show_track = false;
bool g_ntp_started = false;

void clearDisplayFlags() {
  g_radar_visible = false;
  g_idle_visible = false;
  g_track_visible = false;
}

void showRadar() {
  ui::radarDisplayDraw();
  g_radar_visible = true;
  g_idle_visible = false;
  g_track_visible = false;
}

void showIdle() {
  if (services::weather::needsRefresh(config::kWeatherRefreshMs)) {
    services::weather::fetchUpdate(services::location::lat(),
                                   services::location::lon());
    ui::nightDimInvalidate();
  }
  ui::idleClockDraw();
  g_idle_visible = true;
  g_radar_visible = false;
  g_track_visible = false;
}

void showTracked() {
  ui::trackedFlightDraw();
  g_track_visible = true;
  g_radar_visible = false;
  g_idle_visible = false;
}

bool isManualHold() {
  return g_manual == ManualScreen::Clock || g_manual == ManualScreen::Track;
}

/**
 * Auto empty-sky: flip clock ↔ track when a manual track is active.
 * Returns true if a full-screen paint happened (caller should skip tick redraw).
 */
bool showAutoEmpty() {
  if (!services::flight_track::isActive()) {
    g_auto_show_track = false;
    g_auto_flip_ms = 0;
    if (config::kIdleClockWhenEmpty) {
      if (!g_idle_visible) {
        showIdle();
        return true;
      }
    } else if (!g_radar_visible) {
      showRadar();
      return true;
    }
    return false;
  }

  const unsigned long now = millis();
  bool flipped = false;
  if (g_auto_flip_ms == 0 ||
      (now - g_auto_flip_ms) >= config::kAutoTrackFlipMs) {
    if (g_auto_flip_ms != 0) {
      g_auto_show_track = !g_auto_show_track;
      flipped = true;
    } else {
      g_auto_show_track = true;  // prefer track first when a flight is watched
      flipped = true;
    }
    g_auto_flip_ms = now;
  }

  const bool want_track =
      g_auto_show_track || !config::kIdleClockWhenEmpty;

  // Only full-redraw when the visible screen must change (avoids ADS-B-tick flicker).
  if (want_track) {
    if (!g_track_visible || flipped) {
      // Refresh data before the first paint so tick doesn't immediately repaint.
      services::flight_track::pollUpdate(services::location::lat(),
                                         services::location::lon(),
                                         /*allow_network=*/true);
      showTracked();
      return true;
    }
  } else if (!g_idle_visible || flipped) {
    showIdle();
    return true;
  }
  return false;
}

void pollTrackIfNeeded() {
  if (!services::flight_track::isActive()) {
    return;
  }
  // Network callsign polls only while the track screen is up.
  services::flight_track::pollUpdate(services::location::lat(),
                                     services::location::lon(),
                                     g_track_visible);
}

void showBestScreen() {
  if (WiFi.status() != WL_CONNECTED) {
    clearDisplayFlags();
    return;
  }

  // Manual clock/track stick until double-tap returns to auto.
  if (g_manual == ManualScreen::Clock) {
    showIdle();
    return;
  }
  if (g_manual == ManualScreen::Track) {
    showTracked();
    return;
  }

  const size_t inside = ui::radarDisplayInsideCount();
  if (inside > 0) {
    showRadar();
    return;
  }
  showAutoEmpty();
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  // Range only affects the radar map; keep a manual clock/track screen as-is.
  if (WiFi.status() == WL_CONNECTED && g_manual == ManualScreen::Auto &&
      (g_radar_visible || g_idle_visible || g_track_visible)) {
    showBestScreen();
  }
}

void onScreenCycleTap() {
  // Cycle: Auto → Clock → Track → Auto
  if (g_manual == ManualScreen::Auto) {
    g_manual = ManualScreen::Clock;
  } else if (g_manual == ManualScreen::Clock) {
    g_manual = ManualScreen::Track;
  } else {
    g_manual = ManualScreen::Auto;
  }

  if (g_manual == ManualScreen::Clock) {
    Serial.println("Screen: clock (ADS-B stream paused)");
  } else if (g_manual == ManualScreen::Track) {
    Serial.println("Screen: track (ADS-B stream paused)");
  } else {
    Serial.println("Screen: auto (ADS-B stream on)");
    g_auto_flip_ms = 0;
  }

  if (WiFi.status() == WL_CONNECTED) {
    showBestScreen();
  }
}

void onNightDimTap() {
  if (!ui::nightDimToggleForce()) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED &&
      (g_radar_visible || g_idle_visible || g_track_visible)) {
    showBestScreen();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();

  const unsigned long now = millis();
  const uint8_t taps = bootButtonConsumeTapCount();
  if (taps > 0) {
    g_pending_tap_count =
        static_cast<uint8_t>(std::min(10, g_pending_tap_count + taps));
    g_pending_tap_ms = now;
    return;
  }

  if (g_pending_tap_ms == 0) {
    return;
  }
  if ((now - g_pending_tap_ms) < config::kBootMultiTapMs) {
    return;
  }

  const uint8_t n = g_pending_tap_count;
  g_pending_tap_ms = 0;
  g_pending_tap_count = 0;

  if (n >= 3) {
    onNightDimTap();
  } else if (n == 2) {
    onScreenCycleTap();
  } else if (n == 1) {
    onRangeTap();
  }
}

/** Manual clock: weather/time only — no local ADS-B HTTPS. */
void updateManualClock() {
  if (!g_idle_visible) {
    showIdle();
    return;
  }
  if (services::weather::needsRefresh(config::kWeatherRefreshMs)) {
    services::weather::fetchUpdate(services::location::lat(),
                                   services::location::lon());
    ui::nightDimInvalidate();
  }
  ui::idleClockTick();
}

/** Manual track: callsign polls only while this screen is shown. */
void updateManualTrack() {
  pollTrackIfNeeded();
  if (!g_track_visible) {
    showTracked();
  } else {
    ui::trackedFlightTick();
  }
}

void fetchAndUpdateAuto() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    handleBootButton();
    return;
  }

  // Local-list match for a tracked callsign (no extra HTTP). Network polls
  // only if the track pane is currently visible after screen selection.
  const size_t inside = ui::radarDisplayInsideCount();
  if (inside > 0) {
    if (g_idle_visible || g_track_visible) {
      Serial.printf("Radar: %u aircraft in range\n",
                    static_cast<unsigned>(inside));
    }
    // Opportunity to update track from area snapshot without callsign API.
    if (services::flight_track::isActive()) {
      services::flight_track::pollUpdate(services::location::lat(),
                                         services::location::lon(),
                                         /*allow_network=*/false);
    }
    ui::radarDisplayRefreshAircraft();
    g_radar_visible = true;
    g_idle_visible = false;
    g_track_visible = false;
    g_auto_flip_ms = 0;
  } else {
    const bool painted = showAutoEmpty();
    // Between flips: light updates only (skip tick right after a full paint).
    if (!painted) {
      if (g_track_visible) {
        pollTrackIfNeeded();
        ui::trackedFlightTick();
      } else if (g_idle_visible) {
        if (services::weather::needsRefresh(config::kWeatherRefreshMs)) {
          services::weather::fetchUpdate(services::location::lat(),
                                         services::location::lon());
          ui::nightDimInvalidate();
        }
        ui::idleClockTick();
      }
    }
  }
  handleBootButton();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::flight_track::init();
  // Do not setPollFn(wifiLoop): WiFiManager::process during TLS corrupts ADS-B reads.

  if (wifiSetupConnect()) {
    services::time_sync::begin();
    g_ntp_started = true;
    services::weather::fetchUpdate(services::location::lat(),
                                   services::location::lon());
    ui::nightDimInvalidate();
    // Give SNTP a short window before first paint (offset no longer restarts NTP).
    const unsigned long ntp_wait_start = millis();
    while (!services::time_sync::isSynced() &&
           millis() - ntp_wait_start < 3000UL) {
      delay(50);
      wifiLoop();
    }
    showBestScreen();
    pollTrackIfNeeded();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();
  if (ui::nightDimTick()) {
    // Day↔night palette changed — repaint the active screen.
    if (g_radar_visible || g_idle_visible || g_track_visible) {
      showBestScreen();
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible || g_idle_visible || g_track_visible) {
      Serial.println("WiFi lost — will reconnect");
      clearDisplayFlags();
      g_ntp_started = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        if (!g_ntp_started) {
          services::time_sync::begin();
          g_ntp_started = true;
        }
        showBestScreen();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_ntp_started) {
      services::time_sync::begin();
      g_ntp_started = true;
    }
    services::time_sync::retryIfNeeded();
    if (!g_radar_visible && !g_idle_visible && !g_track_visible) {
      showBestScreen();
    }

    const unsigned long now = millis();
    if (isManualHold()) {
      const unsigned long interval =
          (g_manual == ManualScreen::Track &&
           services::flight_track::status().phase ==
               services::flight_track::Phase::Searching)
              ? config::kFlightTrackSearchPollMs
              : config::kFlightTrackPollMs;
      if (now - g_last_manual_poll_ms >= interval) {
        g_last_manual_poll_ms = now;
        if (g_manual == ManualScreen::Clock) {
          updateManualClock();
        } else {
          updateManualTrack();
        }
      } else if (g_idle_visible) {
        ui::idleClockTick();
      } else if (g_track_visible) {
        // Still run maybeAutoEnd / local match on a short tick while held.
        if (services::flight_track::isActive()) {
          services::flight_track::pollUpdate(services::location::lat(),
                                             services::location::lon(),
                                             /*allow_network=*/false);
        }
        ui::trackedFlightTick();
      }
    } else if (now - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = now;
      fetchAndUpdateAuto();
    } else if (g_idle_visible || g_track_visible) {
      // Between area fetches: flip clock↔track; tick only if we did not just paint.
      if (g_manual == ManualScreen::Auto &&
          services::flight_track::isActive() &&
          ui::radarDisplayInsideCount() == 0) {
        if (!showAutoEmpty()) {
          if (g_track_visible) {
            pollTrackIfNeeded();
            ui::trackedFlightTick();
          } else if (g_idle_visible) {
            ui::idleClockTick();
          }
        }
      } else if (g_idle_visible) {
        ui::idleClockTick();
      } else if (g_track_visible) {
        pollTrackIfNeeded();
        ui::trackedFlightTick();
      }
    }
  }

  delay(10);
}
