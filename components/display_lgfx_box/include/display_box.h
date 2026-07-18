// display_box.h — public API for the Box-3 LGFX display driver.
//
// Main.cpp calls display_lgfx_box::init(cfg, display) once from
// setup(), which:
//   * Sets up the LGFX panel (init, rotation, color depth, backlight).
//   * Fills in the geometry portion of TiHostConfig (cols, rows,
//     char_w/h, screen_w/h, display_x/y_offset).
//   * Wires the panel-specific TiDisplay hooks (hostPushCell,
//     hostFillRect, hostPutPixel, hostFillScreen, hostFlush,
//     hostFillBackground).
//
// Feature-specific hooks (hostHonk, hostPostScroll, hostReadBleKey,
// hostPaintBorder) stay in main.cpp — they depend on non-display
// hardware (audio codec, sprite state, BLE keyboard).
//
// The tft object itself is exposed via `extern LGFX tft;` in this
// header for now — box's boot screen + pairing UI still call
// tft.setCursor / setTextColor / print directly. When those move
// into host_common the extern goes away.

#pragma once

// LGFX_AUTODETECT must be defined before <LovyanGFX.hpp> so the
// runtime-probe LGFX class body compiles in every TU that instantiates
// or references `tft`. Without it, the LGFX class body differs between
// this component's .cpp (which owns `tft`) and main.cpp (which uses
// `tft` via the `using` below) — an ODR mismatch that silently
// corrupts vtable/layout and leaves the panel uninitialized.
// Defining here rather than only in main.cpp guarantees both TUs get
// the same class regardless of who included the header first.
#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include "ti_host.h"

namespace display_lgfx_box
{

// LGFX AUTODETECT picks the right panel driver at runtime for the
// Box-3 SPI screen. tft's underlying panel + brightness PWM are
// managed by LGFX; init() drives the wake-up sequence.
extern LGFX tft;

// One-shot init. Sets up the panel, fills cfg with the box geometry
// (32x24 char grid at 8x8 in a 320x240 panel centered at 32,24),
// and populates the display hook function pointers. Returns false if
// tft.init() fails (no observed failure path in practice).
bool init(tihost::TiHostConfig& cfg, tihost::TiDisplay& display);

} // namespace display_lgfx_box
