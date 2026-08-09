/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/flight_track.h"
#include "services/radar_location.h"
#include "services/time_sync.h"
#include "services/weather_client.h"
#include "services/wifi_setup.h"
#include "ui/idle_clock.h"
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
  if (services::flight_track::isActive()) {
    showTracked();
    return;
  }
  if (config::kIdleClockWhenEmpty) {
    showIdle();
  } else {
    showRadar();
  }
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
  }

  if (WiFi.status() == WL_CONNECTED) {
    showBestScreen();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();

  const unsigned long now = millis();
  const uint8_t taps = bootButtonConsumeTapCount();

  if (taps >= 2) {
    g_pending_tap_ms = 0;
    onScreenCycleTap();
    return;
  }

  if (taps == 1) {
    if (g_pending_tap_ms != 0 &&
        (now - g_pending_tap_ms) < config::kBootDoubleTapMs) {
      g_pending_tap_ms = 0;
      onScreenCycleTap();
      return;
    }
    g_pending_tap_ms = now;
    return;
  }

  if (g_pending_tap_ms != 0 &&
      (now - g_pending_tap_ms) >= config::kBootDoubleTapMs) {
    g_pending_tap_ms = 0;
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
  }
  ui::idleClockTick();
}

/** Manual track: only the tracked-callsign poll — no local area stream. */
void updateManualTrack() {
  if (services::flight_track::isActive()) {
    services::flight_track::pollUpdate(services::location::lat(),
                                       services::location::lon());
  }
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

  if (services::flight_track::isActive()) {
    delay(1100);
    services::flight_track::pollUpdate(services::location::lat(),
                                       services::location::lon());
  }

  const size_t inside = ui::radarDisplayInsideCount();
  if (inside > 0) {
    if (g_idle_visible || g_track_visible) {
      Serial.printf("Radar: %u aircraft in range\n",
                    static_cast<unsigned>(inside));
    }
    ui::radarDisplayRefreshAircraft();
    g_radar_visible = true;
    g_idle_visible = false;
    g_track_visible = false;
  } else if (services::flight_track::isActive()) {
    if (!g_track_visible) {
      Serial.println("Track: showing flight status");
      showTracked();
    } else {
      ui::trackedFlightTick();
    }
  } else if (config::kIdleClockWhenEmpty) {
    if (!g_idle_visible) {
      Serial.println("Idle: no traffic — clock/weather");
      showIdle();
    } else {
      if (services::weather::needsRefresh(config::kWeatherRefreshMs)) {
        services::weather::fetchUpdate(services::location::lat(),
                                       services::location::lon());
      }
      ui::idleClockTick();
    }
  } else {
    showRadar();
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
  services::adsb::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    services::time_sync::begin();
    g_ntp_started = true;
    services::weather::fetchUpdate(services::location::lat(),
                                   services::location::lon());
    if (services::flight_track::isActive()) {
      services::flight_track::pollUpdate(services::location::lat(),
                                         services::location::lon());
    }
    showBestScreen();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();

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
    if (!g_radar_visible && !g_idle_visible && !g_track_visible) {
      showBestScreen();
    }

    const unsigned long now = millis();
    if (isManualHold()) {
      // Lighter poll while ADS-B area stream is paused.
      if (now - g_last_manual_poll_ms >= config::kFlightTrackPollMs) {
        g_last_manual_poll_ms = now;
        if (g_manual == ManualScreen::Clock) {
          updateManualClock();
        } else {
          updateManualTrack();
        }
      } else if (g_idle_visible) {
        ui::idleClockTick();
      } else if (g_track_visible) {
        ui::trackedFlightTick();
      }
    } else if (now - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = now;
      fetchAndUpdateAuto();
    } else if (g_idle_visible) {
      ui::idleClockTick();
    } else if (g_track_visible) {
      ui::trackedFlightTick();
    }
  }

  delay(10);
}
