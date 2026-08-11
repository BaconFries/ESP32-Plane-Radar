#pragma once

#include <cstdint>

namespace ui {

/** True after sunset (or fallback night hours) until sunrise, or when forced. */
bool nightDimActive();

/**
 * color565 with optional night scaling of bright channels.
 * Backgrounds stay dark; pass scale_fg=false for near-black fills.
 */
uint16_t themeColor565(uint8_t r, uint8_t g, uint8_t b, bool scale_fg = true);

/**
 * Call from the main loop. Returns true when day↔night flipped so UIs
 * should full-redraw with the new palette.
 */
bool nightDimTick();

/** Force a day/night re-check (e.g. after weather/tz update). */
void nightDimInvalidate();

/**
 * Manually toggle night theme (BOOT triple-tap). Locks out auto sun schedule
 * until reboot; returns true if the active palette changed.
 */
bool nightDimToggleForce();

}  // namespace ui
