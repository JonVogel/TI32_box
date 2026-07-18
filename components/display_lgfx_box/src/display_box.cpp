// display_box.cpp — Box-3 LGFX panel init + TiDisplay hook wrappers.
//
// See display_box.h for the module contract. Everything in this file
// used to live in TI32-box/main/main.cpp; consolidating it here means
// (1) main.cpp gets ~150 lines shorter, (2) the LGFX + tft +
// hostPushCell family are packaged as one swappable unit, and (3)
// future panel variants (a Box-2 module, a Box-3 revision B with a
// different display, etc.) can appear as sibling components without
// touching main.cpp.

#include <Arduino.h>
#include "display_box.h"

namespace display_lgfx_box
{

LGFX tft;

// Same box screen geometry constants that used to live in main.cpp.
// TI grid is always 32x24; box-3 SPI panel is 320x240; center the
// 256x192 (native 8x8) grid with 32-px left/right and 24-px top/
// bottom margins.
static constexpr int kCols            = 32;
static constexpr int kRows            = 24;
static constexpr int kCharW           = 8;
static constexpr int kCharH           = 8;
static constexpr int kScreenW         = 320;
static constexpr int kScreenH         = 240;
static constexpr int kDisplayXOffset  = 32;
static constexpr int kDisplayYOffset  = 24;

// Panel-side hook wrappers. Kept file-static so nothing outside this
// module can grab their addresses directly — the only intended path
// is via the TiDisplay pointers we hand to hostCommonInit() from
// init() below.
static void hostPushCell_impl(int px, int py, int w, int h,
                              const uint16_t* pixels)
{
  tft.pushImage(px, py, w, h, pixels);
}
static void hostFillRect_impl(int px, int py, int w, int h, uint16_t color)
{
  tft.fillRect(px, py, w, h, color);
}
static void hostPutPixel_impl(int px, int py, uint16_t color)
{
  tft.drawPixel(px, py, color);
}
static void hostFillScreen_impl(uint16_t color)
{
  tft.fillScreen(color);
}
static void hostFlush_impl() { /* LovyanGFX SPI is direct-to-panel */ }
// Box's whole-panel background paint is trivially a fillScreen — no
// letterbox area (Box-3 grid fills the entire panel), no border ring.
static void hostFillBackground_impl(uint16_t bg) { tft.fillScreen(bg); }

bool init(tihost::TiHostConfig& cfg, tihost::TiDisplay& display)
{
  tft.init();
  tft.setRotation(1);            // landscape, USB on the right
  tft.setColorDepth(16);
  tft.setSwapBytes(true);        // pushImage data is little-endian
                                 // uint16_t; panel wants big-endian
                                 // RGB565.
  tft.setBrightness(192);        // 0..255 backlight PWM duty
  // Initial screen fill is done later after host_common's palette
  // + bgColor are ready (via gfxResetColors + tiClearScreen). Leave
  // the panel dark for now so we don't briefly flash a wrong color.
  tft.setTextSize(1);

  cfg.cols             = kCols;
  cfg.rows             = kRows;
  cfg.char_w           = kCharW;
  cfg.char_h           = kCharH;
  cfg.screen_w         = kScreenW;
  cfg.screen_h         = kScreenH;
  cfg.display_x_offset = kDisplayXOffset;
  cfg.display_y_offset = kDisplayYOffset;
  // Feature flags stay in main.cpp — they describe non-display
  // hardware (audio, BLE, WiFi, pairing UI) that this module doesn't
  // know or care about.

  display.hostBegin          = nullptr;
  display.hostPushCell       = hostPushCell_impl;
  display.hostFillRect       = hostFillRect_impl;
  display.hostPutPixel       = hostPutPixel_impl;
  display.hostFillScreen     = hostFillScreen_impl;
  display.hostFlush          = hostFlush_impl;
  display.hostFillBackground = hostFillBackground_impl;
  // hostPaintBorder / hostHonk / hostPostScroll / hostReadBleKey
  // stay per-host — main.cpp fills them in after init() returns.

  return true;
}

} // namespace display_lgfx_box
