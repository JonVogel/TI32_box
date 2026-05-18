/*
 * BLE gamepad → CALL JOYST adapter for the TI BASIC simulator.
 *
 * Designed for the 8BitDo Zero 2 (and similar BLE HID gamepads) but
 * permissive enough to handle whatever bytes the device sends. Two
 * paths feed joyX/joyY:
 *
 *  1) Real gamepad reports (bleGpOnReport): parse stick/hat bytes.
 *  2) Arrow keys from the BLE keyboard (bleKbReportArrowState):
 *     mirrors arrow key press/release into joyX/joyY too. This makes
 *     CALL JOYST work in "keyboard mode" on the Zero 2 (D-pad sends
 *     arrow keys) and also from a regular USB keyboard.
 *
 * TI convention for CALL JOYST(unit, X, Y):
 *   X = -4 (left), 0 (center), +4 (right)
 *   Y = -4 (down), 0 (center), +4 (up)
 * Only unit 1 is supported — single-pad scope.
 */

#pragma once

#include <Arduino.h>

// Set to 1 to print raw gamepad reports to Serial — useful for figuring
// out the byte layout of an unknown gamepad. Off by default.
#define BLE_GP_DEBUG 0

static volatile int joyX = 0;
static volatile int joyY = 0;
static volatile bool joyFire = false;

// Last 4 arrow keys held (true while pressed). Set by ble_keyboard.h's
// per-frame report handler so JOYST mirrors the keyboard.
static volatile bool joyArrowUp    = false;
static volatile bool joyArrowDown  = false;
static volatile bool joyArrowLeft  = false;
static volatile bool joyArrowRight = false;

// Recompose joyX/joyY from arrow state. Called by ble_keyboard.h after
// it scans each report for held arrows.
static inline void bleGpUpdateFromArrows()
{
  int nx = 0, ny = 0;
  if (joyArrowLeft)  nx = -4;
  if (joyArrowRight) nx = +4;
  if (joyArrowDown)  ny = -4;
  if (joyArrowUp)    ny = +4;
  joyX = nx;
  joyY = ny;
}

// Try to parse a non-keyboard HID report as gamepad input. The 8BitDo
// Zero 2 in D-input mode tends to lay reports out roughly:
//   bytes 0..3   stick X / Y (or padding for digital-only sticks)
//   byte  4 or 5 hat switch (0..7 = N..NW, 0xF = center) + buttons
// We try the analog interpretation first; if both axes are centered we
// fall back to the hat byte.
static inline void bleGpOnReport(const uint8_t* report, size_t len)
{
  // Skip standard keyboard reports — they're 8 bytes with [mod, 0, k0..k5]
  // and the keyboard handler covers them.
  if (len == 8) return;
  if (len == 0) return;

#if BLE_GP_DEBUG
  Serial.printf("GP[%u]:", (unsigned)len);
  for (size_t i = 0; i < len && i < 16; i++) Serial.printf(" %02X", report[i]);
  Serial.println();
#endif

  int newX = 0, newY = 0;

  // Analog stick interpretation
  if (len >= 2)
  {
    uint8_t bx = report[0];
    uint8_t by = report[1];
    if (bx < 0x40)        newX = -4;
    else if (bx > 0xC0)   newX = +4;
    if (by < 0x40)        newY = +4;   // up = +4 on TI
    else if (by > 0xC0)   newY = -4;
  }

  // Hat-byte fallback if axes were centered. Try a few likely byte
  // positions — different BLE gamepads put the hat at different offsets.
  if (newX == 0 && newY == 0)
  {
    int hatOff = -1;
    if (len > 5)      hatOff = 5;
    else if (len > 4) hatOff = 4;
    else if (len > 2) hatOff = 2;
    if (hatOff >= 0)
    {
      uint8_t hat = report[hatOff] & 0x0F;
      switch (hat)
      {
        case 0: newY = +4; break;
        case 1: newY = +4; newX = +4; break;
        case 2: newX = +4; break;
        case 3: newY = -4; newX = +4; break;
        case 4: newY = -4; break;
        case 5: newY = -4; newX = -4; break;
        case 6: newX = -4; break;
        case 7: newY = +4; newX = -4; break;
        default: break;
      }
    }
  }

  // Only overwrite the keyboard-arrow contribution if the gamepad is
  // actually pushing a direction. A centered gamepad shouldn't cancel a
  // held arrow key.
  if (newX != 0 || newY != 0)
  {
    joyX = newX;
    joyY = newY;
  }
  else
  {
    bleGpUpdateFromArrows();
  }
}

// CALL JOYST queries. unit 1 → real state; others → centered.
static inline int bleGpJoystickX(int unit)
{
  return (unit == 1) ? joyX : 0;
}
static inline int bleGpJoystickY(int unit)
{
  return (unit == 1) ? joyY : 0;
}
