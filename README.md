# Plane Radar

![plane-radar](https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419)

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](https://github.com/MatixYo/ESP32-Plane-Radar/releases) · **End-user guide:** [USER_GUIDE.md](USER_GUIDE.md)

Firmware for an **ESP32-C3 Super Mini** and a **1.28″ round GC9A01** display (240×240). Shows a circular **ADS-B radar** around your configured location, with **WiFiManager** for first-time setup.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on AP `PlaneRadar-Setup`
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid
3. **Idle clock** — when nothing is inside the outer ring, show local time + current weather ([Open-Meteo](https://open-meteo.com/)); returns to radar when traffic appears
4. **Live traffic page** — `http://plane-radar.local/traffic` lists callsigns from the last ADS-B fetch
5. **Track a flight** — `http://plane-radar.local/track` to follow a callsign for the rest of the flight (status on the display when the local radar is empty)

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop with periodic ADS-B updates (~3 s).

## Controls (BOOT, GPIO 9, active LOW)


| Action         | Effect                                                                                                                                |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| **Short tap**  | Cycle range preset (5 → 10 → 15 → 25 km); saved to flash                                                                              |
| **Double tap** | Cycle screen mode: **auto** (live ADS-B + flip) → **clock** (stream paused) → **track** (stream paused; tracked callsign only) → auto |
| **Triple tap** | Toggle **night theme** (dimmer colors) for testing; locks until reboot                                                                |
| **Hold 3 s**   | Clear Wi‑Fi, location, and units; reboot into setup portal                                                                            |


During setup you can also hold BOOT at power-on to force a credential reset (same as the long press).

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to `PlaneRadar-Setup`
2. Open `http://plane-radar.local` (preferred) or `http://192.168.4.1` — both are shown on the yellow setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

**Reconfigure anytime** (after the device is on your network):

1. Open `http://plane-radar.local` or `http://<device-ip>` (e.g. from your router or serial log at boot)
2. The portal home lists **Live traffic** and **Track a flight** with full bookmarkable URLs, plus Setup / Wi‑Fi
3. Change Wi‑Fi, location (ZIP or lat/lon), units, or runways under **Setup**; save
4. Open `http://plane-radar.local/traffic` for callsign / type / altitude / track / ground speed from the current ADS-B snapshot (auto-refresh every 5 s)
5. Open `http://plane-radar.local/track` to follow one callsign; optionally enter origin/destination (e.g. LGA / PBI) because public schedule DBs are often wrong for “today”. The round display shows status while airborne (and the local radar is empty)

The same portal runs on the setup AP and on the device’s LAN IP while connected to Wi‑Fi. mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly; use the IP if needed.

**Custom fields** (stored in NVS):


| Field                          | Purpose                                                                                                                                                                             |
| ------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **US ZIP code**                | Preferred; if set (and the phone/browser or device has internet), resolved to lat/lon via [Zippopotam.us](https://zippopotam.us/) before save. Either ZIP **or** lat/lon is enough. |
| **Latitude / Longitude**       | Radar center and ADS-B query position (defaults in `config.h` until set). Overwritten automatically when ZIP lookup succeeds.                                                       |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`)                                                                                                                 |
| **Show airport runways**       | Major-airport runway overlay on the radar (off to hide)                                                                                                                             |


**Location:** Open **Setup** in the portal menu (or the link on **Info** → Setup). Enter a US ZIP **or** lat/lon, then save. A 5-digit ZIP overrides the lat/lon fields (including factory defaults). ZIP lookup needs internet on the phone/browser (or on the device when already on Wi‑Fi). Firmware Update/Browse is disabled.

After a reset, the device reboots and shows the setup screen immediately (no “Connecting” loop on stale credentials).

## Radar display



### Grid

- Dark blue background with subdued green rings and crosshairs
- White **N / S / E / W** at the bezel; range label on the **east** spoke (ring 3 = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`.

### Range presets


| Ring 3 label  | Outer radius (aircraft scale) |
| ------------- | ----------------------------- |
| 5 km / 3 mi   | ~6.7 km                       |
| 10 km / 6 mi  | ~13.3 km (default)            |
| 15 km / 9 mi  | ~20 km                        |
| 25 km / 16 mi | ~33.3 km                      |


Preset, miles/km, and overlay toggles persist across reboot (`planeradar` NVS namespace).

### Runways

- Major airports from OurAirports (`large_airport`); all open runway strips in range (helipads excluded)
- Teal runway lines with one ICAO label per airport (e.g. `KJFK`); toggle in the Wi‑Fi setup portal
- Update the embedded list: `python3 scripts/build_large_airports.py`



### Aircraft

- **Inside the outer ring** — red heading triangle, magenta speed vector (clipped at the ring), callsign / type / altitude tags
- **Outside the ring** (still within ADS-B fetch) — small **red dot on the screen rim** at the correct bearing (direction cue; not distance-accurate past the ring)
- **Tags** — placed toward the **center**: west (left) → tag on the **right** of the symbol; east (right) → tag on the **left**
- **Idle** — if no aircraft are inside the outer ring, the display shows **local time**, date, and **current weather** (temp + short condition). Timezone comes from Open-Meteo for your radar center; °F when miles are enabled, otherwise °C. Weather refreshes about every 30 minutes.
- **Tracked flight** — if you started a track on the portal and nothing is in the local ring, **Auto** flips between the **clock** and the **track** screen (~20 s). Double-tap BOOT for a sticky clock or track view. Callsign HTTP runs **only while the track screen is visible**: every **15 min** while searching (no ADS-B yet), then every few seconds once the aircraft is seen. After landing, polls stop and the track clears after **2 hours**. Optional origin/destination on `/track` set the route (public schedule DBs are often wrong). If the tracked plane enters the local radar, it is drawn in yellow.

As range decreases (or aircraft approach), targets move inward; beyond-ring dots become full symbols when they cross the outer ring. Rim-only traffic does **not** keep the radar awake — only in-ring aircraft do.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/`
- Fetch radius: `ui::radar::fetchRadiusKm()` — scales with the active preset to roughly the screen edge (so rim dots have data)
- Poll interval: `kAdsbFetchIntervalMs` (5 s) in `config.h`
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)



## Configuration

Edit `include/config.h` for hardware and behavior:


| Area             | Keys / notes                                                                                                                                               |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Portal           | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`)                                           |
| Wi‑Fi timing     | connect attempts, reconnect grace, portal timeout (`0` = no timeout)                                                                                       |
| BOOT             | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs`                                                                                                            |
| Display SPI      | pins, `kDisplayInvert`, `kDisplayRgbOrder`, `kDisplaySpiWriteHz`, night color scale (`kNightFgScale`)                                                      |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides)                                                                                            |
| ADS-B            | `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft`                                                                                                          |
| Idle / weather   | `kIdleClockWhenEmpty`, `kWeatherRefreshMs`, `kAutoTrackFlipMs`                                                                                             |
| Flight track     | `kFlightTrackSearchPollMs` (15 min while searching), `kFlightTrackPollMs`, `kFlightTrackLostMs`, `kFlightTrackLandedMs` (2 h hold), `kFlightTrackSearchMs` |


Range presets: `include/ui/radar_range.h` (`kRangePresets`).

## Project layout

```
include/
  config.h
  hardware/
    lgfx_config.hpp
    display.h
    display_font.h
  data/
    large_airports.h
  ui/
    radar_theme.h
    radar_range.h
    radar_display.h
    runway_overlay.h
    status_screens.h
  services/
    wifi_setup.h
    radar_location.h
    adsb_client.h
    flight_track.h
    time_sync.h
    weather_client.h
    portal_location_head.h
data/
  ui_font.vlw              — embedded smooth UI font (Noto Sans Bold)
scripts/
  build_large_airports.py
src/
  main.cpp
  data/
    large_airports_data.cpp
  hardware/
  ui/
  services/
```



## Wiring (GC9A01 ↔ ESP32-C3 Super Mini)


| Display     | ESP32-C3    |
| ----------- | ----------- |
| VCC         | 3V3         |
| GND         | GND         |
| RST         | GPIO **0**  |
| CS          | GPIO **1**  |
| DC          | GPIO **10** |
| SDA (MOSI)  | GPIO **3**  |
| SCL (SCLK)  | GPIO **4**  |
| BOOT (user) | GPIO **9**  |


**Night dim:** After time sync, bright UI colors (clock green, labels, aircraft tags) are scaled down from **sunset→sunrise** (Open-Meteo; fallback 21:00–07:00). No backlight PWM — works with BLK tied to 3V3. Tune with `kNightFgScale` in `config.h`.

## Build

```bash
pio run -t upload
pio device monitor
```

- PlatformIO env: `supermini`
- Serial: **115200** baud
- USB CDC on boot enabled in `platformio.ini` for the Super Mini



### Web-flashable release image

Single `.bin` for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools (ESP32-C3, 4 MB, flash at **0x0**):

```bash
chmod +x scripts/merge-firmware.sh   # once
./scripts/merge-firmware.sh
```

Writes `release/plane-radar-merged.bin`. Skip rebuild if firmware is already built:

```bash
./scripts/merge-firmware.sh --no-build
```

Or via PlatformIO only (output: `.pio/build/supermini/firmware-merged.bin`):

```bash
pio run -e supermini
pio run -t merge -e supermini
```

Put the board in download mode (hold **BOOT**, tap **RESET**), then flash with Chrome/Edge over USB.

### CI and releases (GitHub Actions)


| Workflow                                 | When                         | Output                                                                   |
| ---------------------------------------- | ---------------------------- | ------------------------------------------------------------------------ |
| [Build](.github/workflows/build.yml)     | Push / PR to `main`          | Artifact `plane-radar-supermini` (merged + split `.bin` files, ~90 days) |
| [Release](.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release asset `plane-radar-v1.0.0.bin` + `.sha256`                |


To ship a version users can download:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds firmware in CI and attaches the merged image to the release. Download from **Releases** on GitHub, then flash at **0x0** (ESP32-C3, 4 MB).

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

