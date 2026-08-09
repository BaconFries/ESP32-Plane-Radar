#pragma once

namespace ui {

/** Full-screen idle clock + weather (no traffic in range). */
void idleClockDraw();

/**
 * Update clock digits if the minute changed (cheap partial refresh).
 * Returns true if the screen was redrawn.
 */
bool idleClockTick();

}  // namespace ui
