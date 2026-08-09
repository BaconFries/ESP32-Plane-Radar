#pragma once

#include <cstddef>

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/** Aircraft currently drawn as symbols inside the outer ring. */
size_t radarDisplayInsideCount();

/**
 * Free the ~112 KB frame sprite so HTTPS/ADS-B can allocate SSL buffers.
 * Next draw recreates it.
 */
void radarDisplayReleaseFrameBuffer();

}  // namespace ui
