#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;
/** Second tap within this window counts as a double-tap (screen cycle). */
constexpr unsigned long kBootDoubleTapMs = 550UL;
/** Wait this long after the last tap before committing 1 / 2 / 3+ tap actions. */
constexpr unsigned long kBootMultiTapMs = 550UL;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

/**
 * Night UI: scale bright RGB channels (no backlight PWM — BLK often tied to 3V3).
 * 255 = full day colors; ~90 ≈ 35% for a softer night look.
 */
constexpr uint8_t kNightFgScale = 90;
/** How often to re-evaluate day/night (sun times / clock). */
constexpr unsigned long kNightDimCheckMs = 30UL * 1000UL;
/**
 * If sunrise/sunset unknown: treat as night from this local hour (inclusive)
 * until kNightEndHour (exclusive).
 */
constexpr int kNightStartHour = 21;
constexpr int kNightEndHour = 7;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 5000;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

/** Open-Meteo refresh while idle / on first empty radar. */
constexpr unsigned long kWeatherRefreshMs = 30UL * 60UL * 1000UL;
/** Show clock/weather when no aircraft inside the outer ring. */
constexpr bool kIdleClockWhenEmpty = true;
/** In Auto with an active track and empty radar: flip clock ↔ track this often. */
constexpr unsigned long kAutoTrackFlipMs = 20UL * 1000UL;

/**
 * Portal-tracked flight polling (only while the track screen is visible).
 * Searching (no ADS-B yet): slow callsign poll.
 * Airborne / seen: faster ADS-B callsign poll.
 */
constexpr unsigned long kFlightTrackSearchPollMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kFlightTrackPollMs = 5000;
/** Clear track after this long with no ADS-B hit (was seen before). */
constexpr unsigned long kFlightTrackLostMs = 20UL * 60UL * 1000UL;
/** After landing: keep final status this long, then clear (no API after touchdown). */
constexpr unsigned long kFlightTrackLandedMs = 2UL * 60UL * 60UL * 1000UL;
/** Give up searching if never seen. */
constexpr unsigned long kFlightTrackSearchMs = 6UL * 60UL * 60UL * 1000UL;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
