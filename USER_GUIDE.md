# Plane Radar — User Guide

Live ADS-B aircraft on a round display. Case and build files: [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083). Firmware: [Releases](https://github.com/MatixYo/ESP32-Plane-Radar/releases).

## First-time setup

1. Power on the device.
2. On your phone or computer, join Wi‑Fi **`PlaneRadar-Setup`**.
3. Open **http://plane-radar.local** (or **http://192.168.4.1** if that fails). The address is also shown on the yellow setup screen.
4. Enter your home Wi‑Fi and save.
5. Open **Setup** and set your location: a US ZIP code, or latitude/longitude. Optionally choose miles vs kilometers and whether to show airport runways.
6. Save. The device reboots and connects to your network.

After that, it reconnects automatically on power-up.

## Everyday use

| What you see | Meaning |
|--------------|---------|
| **Radar** | Aircraft near you on a circular map |
| **Clock + weather** | No planes inside the outer ring — local time and conditions |
| **Tracked flight** | Status for a callsign you chose on the web (when the local radar is empty) |

Data comes from [adsb.fi](https://opendata.adsb.fi/). Weather and timezone come from [Open-Meteo](https://open-meteo.com/).

## Button (BOOT)

Use the BOOT button on the board (short taps, or hold).

| Action | What it does |
|--------|----------------|
| **Short tap** | Next range: 5 → 10 → 15 → 25 km (saved) |
| **Double tap** | Screen mode: **auto** → **clock** → **track** → auto |
| **Triple tap** | Night theme on/off (until reboot) |
| **Hold ~3 seconds** | Wipe Wi‑Fi and settings; reboot into setup |

You can also hold BOOT while powering on to force setup again.

**Screen modes**

- **Auto** — live radar; when empty, clock (and track screen if you are following a flight)
- **Clock** — clock only; ADS-B paused
- **Track** — tracked-flight screen only; ADS-B paused

## On your network

Open **http://plane-radar.local** (or the device IP from your router).

| Page | Use |
|------|-----|
| **Home** | Links to traffic, track, and setup |
| **/traffic** | Live list of nearby callsigns (refreshes every 5 s) |
| **/track** | Follow one callsign for the rest of the flight |
| **Setup** | Wi‑Fi, location, miles/km, runways |

If **plane-radar.local** does not resolve, use the IP address instead.

### Tracking a flight

1. Open **/track** and enter a callsign.
2. Optionally add origin/destination (e.g. LGA / PBI) if the auto route looks wrong.
3. While that flight is airborne and nothing is in your local ring, the display can show track status (in **auto**, it flips with the clock).
4. When the plane enters your radar, it is drawn in **yellow**.
5. After landing, tracking clears after a couple of hours.

## Reading the radar

- **Rings** — distance from your location; label on the east spoke (e.g. `10km` or `6mi`).
- **Red triangle** — aircraft inside the ring (heading); magenta line is speed.
- **Labels** — callsign, type, altitude.
- **Red rim dots** — aircraft outside the ring but still in range (bearing only).
- **Teal lines** — major airport runways (if enabled in Setup).

Rim-only traffic does not keep the radar awake — only planes inside the outer ring do.

## Range presets

| Label on screen | Approx. outer coverage |
|-----------------|------------------------|
| 5 km / 3 mi | ~7 km |
| 10 km / 6 mi | ~13 km (default) |
| 15 km / 9 mi | ~20 km |
| 25 km / 16 mi | ~33 km |

## Tips

- **Night** — After sunset, bright colors dim automatically. Triple-tap BOOT for a sticky night theme until reboot.
- **Reset** — Hold BOOT ~3 s (or hold BOOT at power-on) if you need a clean setup.
- **No traffic** — Check Wi‑Fi, location in Setup, and that the range is large enough for your area.
- Developers / flashing / wiring: see [README.md](README.md).
