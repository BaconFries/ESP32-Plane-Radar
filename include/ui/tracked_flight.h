#pragma once

namespace ui {

/** Full-screen status for the portal-tracked flight. */
void trackedFlightDraw();

/** Redraw if phase/alt/time-sensitive fields changed. */
bool trackedFlightTick();

}  // namespace ui
