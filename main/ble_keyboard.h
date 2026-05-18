/*
 * BLE keyboard → ASCII adapter for the TI BASIC simulator.
 *
 * Thin layer over the BleHidHost library: converts raw HID reports into
 * ASCII characters and pushes them into a small ring buffer that the
 * simulator's input sites (checkInput, getInputLine, gfxReadKey) poll
 * alongside Serial.
 *
 * Press F12 or the BOOT button to enter pairing mode.
 */

#pragma once

#include <BleHidHost.h>
#include "ble_gamepad.h"

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------
#define BLE_KB_BUF_SIZE 64
static volatile uint8_t bleKbBuf[BLE_KB_BUF_SIZE];
static volatile int bleKbHead = 0;
static volatile int bleKbTail = 0;

// Set by bleKbOnReport when CLEAR (12) / ESC (0x1B) / Ctrl+C (0x03) is
// received. exec_manager polls this during RUN so the break works even
// from the BLE keyboard.
static volatile bool bleKbBreakRequested = false;

static inline void bleKbPush(uint8_t c)
{
  int next = (bleKbHead + 1) % BLE_KB_BUF_SIZE;
  if (next != bleKbTail)
  {
    bleKbBuf[bleKbHead] = c;
    bleKbHead = next;
  }
}

static inline bool bleKbAvailable()
{
  return bleKbHead != bleKbTail;
}

static inline int bleKbPeek()
{
  if (bleKbHead == bleKbTail)
  {
    return -1;
  }
  return bleKbBuf[bleKbTail];
}

static inline int bleKbRead()
{
  if (bleKbHead == bleKbTail)
  {
    return -1;
  }
  uint8_t c = bleKbBuf[bleKbTail];
  bleKbTail = (bleKbTail + 1) % BLE_KB_BUF_SIZE;
  return c;
}

// ---------------------------------------------------------------------------
// HID → ASCII
// ---------------------------------------------------------------------------
#define HID_KEY_A          0x04
#define HID_KEY_Z          0x1D
#define HID_KEY_1          0x1E
#define HID_KEY_0          0x27
#define HID_KEY_ENTER      0x28
#define HID_KEY_ESCAPE     0x29
#define HID_KEY_BACKSPACE  0x2A
#define HID_KEY_TAB        0x2B
#define HID_KEY_SPACE      0x2C
#define HID_KEY_CAPSLOCK   0x39
#define HID_KEY_F1         0x3A
#define HID_KEY_F2         0x3B
#define HID_KEY_F3         0x3C
#define HID_KEY_F4         0x3D
#define HID_KEY_F5         0x3E
#define HID_KEY_F6         0x3F
#define HID_KEY_F7         0x40
#define HID_KEY_F8         0x41
#define HID_KEY_F9         0x42
#define HID_KEY_F10        0x43
#define HID_KEY_F11        0x44
#define HID_KEY_F12        0x45
#define HID_KEY_DELETE     0x4C
#define HID_KEY_RIGHT      0x4F
#define HID_KEY_LEFT       0x50
#define HID_KEY_DOWN       0x51
#define HID_KEY_UP         0x52

#define HID_MOD_LSHIFT  0x02
#define HID_MOD_RSHIFT  0x20

static bool bleKbCapsLock = false;

static int bleKbHidToAscii(uint8_t k, bool shift)
{
  if (k >= HID_KEY_A && k <= HID_KEY_Z)
  {
    return shift ? ('A' + (k - HID_KEY_A)) : ('a' + (k - HID_KEY_A));
  }
  if (k >= HID_KEY_1 && k <= HID_KEY_0)
  {
    static const char numUnshift[] = "1234567890";
    static const char numShift[]   = "!@#$%^&*()";
    return shift ? numShift[k - HID_KEY_1] : numUnshift[k - HID_KEY_1];
  }
  switch (k)
  {
    case HID_KEY_ENTER:     return '\r';
    case HID_KEY_ESCAPE:    return 0x1B;
    case HID_KEY_BACKSPACE: return 0x7F;
    case HID_KEY_TAB:       return '\t';
    case HID_KEY_SPACE:     return ' ';
    case 0x2D: return shift ? '_'  : '-';
    case 0x2E: return shift ? '+'  : '=';
    case 0x2F: return shift ? '{'  : '[';
    case 0x30: return shift ? '}'  : ']';
    case 0x31: return shift ? '|'  : '\\';
    case 0x33: return shift ? ':'  : ';';
    case 0x34: return shift ? '"'  : '\'';
    case 0x35: return shift ? '~'  : '`';
    case 0x36: return shift ? '<'  : ',';
    case 0x37: return shift ? '>'  : '.';
    case 0x38: return shift ? '?'  : '/';

    // Cursor keys — match TI FCTN+S/D/E/X (Left/Right/Up/Down) CHR$ codes
    case HID_KEY_LEFT:   return  8;   // FCTN+S
    case HID_KEY_RIGHT:  return  9;   // FCTN+D
    case HID_KEY_DOWN:   return 10;   // FCTN+X
    case HID_KEY_UP:     return 11;   // FCTN+E

    // Function keys — match TI FCTN+1..FCTN+9 (CALL KEY mode 0 codes)
    case HID_KEY_DELETE: return  7;   // same as FCTN+1 DEL
    case HID_KEY_F1:     return  7;   // FCTN+1  DEL
    case HID_KEY_F2:     return  4;   // FCTN+2  INS
    case HID_KEY_F3:     return  2;   // FCTN+3  ERASE
    case HID_KEY_F4:     return 12;   // FCTN+4  CLEAR
    case HID_KEY_F5:     return  5;   // FCTN+5  BEGIN
    case HID_KEY_F6:     return  1;   // FCTN+6  AID
    case HID_KEY_F7:     return  6;   // FCTN+7  PROCEED
    case HID_KEY_F8:     return 14;   // FCTN+8  REDO
    case HID_KEY_F9:     return 15;   // FCTN+9  BACK
    case HID_KEY_F11:    return -1;   // (unmapped; FCTN+= QUIT not implemented)
    // F12 handled separately as BLE pairing trigger
  }
  return -1;
}

// Edge-triggered HID report handler (runs in BLE notify context)
static void bleKbOnReport(const uint8_t* report, size_t len)
{
  // Anything that isn't a standard 8-byte HID keyboard report goes to
  // the gamepad parser. Both the 8BitDo Zero 2's gamepad modes and
  // generic BLE gamepads send variable-length reports here.
  if (len != 8)
  {
    bleGpOnReport(report, len);
    return;
  }
  static uint8_t prevKeys[6] = {0};

  uint8_t modifiers = report[0];
  const uint8_t* keys = &report[2];
  bool shift = (modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;

  // Mirror arrow-key press state into the joystick globals so CALL
  // JOYST works in keyboard mode (Zero 2 mode K) and from any
  // ordinary BLE keyboard.
  bool up = false, down = false, left = false, right = false;
  for (int i = 0; i < 6; i++)
  {
    switch (keys[i])
    {
      case HID_KEY_UP:    up    = true; break;
      case HID_KEY_DOWN:  down  = true; break;
      case HID_KEY_LEFT:  left  = true; break;
      case HID_KEY_RIGHT: right = true; break;
    }
  }
  joyArrowUp    = up;
  joyArrowDown  = down;
  joyArrowLeft  = left;
  joyArrowRight = right;
  bleGpUpdateFromArrows();

  // F12 edge → trigger pairing
  bool f12Now = false, f12Prev = false;
  for (int i = 0; i < 6; i++)
  {
    if (keys[i] == HID_KEY_F12)     f12Now  = true;
    if (prevKeys[i] == HID_KEY_F12) f12Prev = true;
  }
  if (f12Now && !f12Prev)
  {
    BleHidHost::requestPairingMode();
  }

  for (int i = 0; i < 6; i++)
  {
    uint8_t k = keys[i];
    if (k == 0)
    {
      continue;
    }

    bool wasPressed = false;
    for (int j = 0; j < 6; j++)
    {
      if (prevKeys[j] == k)
      {
        wasPressed = true;
        break;
      }
    }
    if (wasPressed)
    {
      continue;
    }

    if (k == HID_KEY_CAPSLOCK)
    {
      bleKbCapsLock = !bleKbCapsLock;
      continue;
    }

    bool effectiveShift = shift;
    if (bleKbCapsLock && k >= HID_KEY_A && k <= HID_KEY_Z)
    {
      effectiveShift = true;
    }

    int ascii = bleKbHidToAscii(k, effectiveShift);
    if (ascii >= 0)
    {
      if (ascii == 12 || ascii == 0x1B || ascii == 0x03)
      {
        bleKbBreakRequested = true;
      }
      bleKbPush((uint8_t)ascii);
    }
  }

  memcpy(prevKeys, keys, 6);
}

// BLE keyboard scan policy. Two modes:
//   TITLE      — aggressive scanning while the user sits on the boot /
//                splash screen. This is the moment a keyboard is most
//                likely to be turned on, so we trade CPU + radio noise
//                for fast discovery. setup() calls bleKbSetMode(BLE_KB_TITLE)
//                implicitly via bleKbInit; .ino flips to INTERACTIVE
//                once the user dismisses the boot screen.
//   INTERACTIVE — radio idle when no keyboard is connected. If a
//                previously-connected keyboard drops out, fire ONE
//                60-second low-duty (10%) background scan window to
//                try to reacquire it. Otherwise stay quiet so the
//                interpreter has full CPU. User can force pairing via
//                CALL PAIR / BOOT button.
//
// This replaces the older "every 5 s, re-arm aggressive pairing-mode
// scan" watchdog that made the radio hammer permanently whenever the
// keyboard was off.
enum BleKbMode : uint8_t
{
  BLE_KB_TITLE       = 0,
  BLE_KB_INTERACTIVE = 1
};
static BleKbMode bleKbMode = BLE_KB_TITLE;

static inline void bleKbSetMode(BleKbMode m)
{
  if (bleKbMode == m) return;
  bleKbMode = m;
  if (m == BLE_KB_INTERACTIVE)
  {
    // Entering interactive mode: if nothing's connected and the title
    // screen's scan didn't find anything, take the radio idle. The
    // task tick below will reopen a background scan only if a bonded
    // peer disconnects later.
    if (!BleHidHost::isConnected())
    {
      BleHidHost::stopScan();
      Serial.println("BleHidHost: interactive mode, radio idle");
    }
  }
}

static inline void bleKbInit()
{
  BleHidHost::setReportCallback(bleKbOnReport);
  BleHidHost::begin("TI99-BASIC", "ti99basic");
  // Boot in TITLE mode — the .ino calls bleKbSetMode(BLE_KB_INTERACTIVE)
  // after showBootScreen() returns.
  bleKbMode = BLE_KB_TITLE;
}

static inline void bleKbTask()
{
  BleHidHost::task();

  if (bleKbMode == BLE_KB_TITLE)
  {
    // While on the title screen, keep scan alive so the user can power
    // on their keyboard and have it connect during the splash.
    // begin() opens a short scan; if it expired without a hit, reopen.
    // Aggressive params: user is sitting and waiting — fast discovery
    // wins over radio noise during this short window.
    if (!BleHidHost::isConnected() &&
        !BleHidHost::inPairingMode())
    {
      BleHidHost::requestSilentScan();
    }
    return;
  }

  // INTERACTIVE mode: only re-engage the radio if a bonded peer was
  // previously connected and just disconnected. Track that edge here
  // so we don't keep firing if the user has never paired anything.
  static bool wasConnected = false;
  static uint32_t backgroundScanAt = 0;
  bool nowConnected = BleHidHost::isConnected();

  if (nowConnected)
  {
    wasConnected = true;
    backgroundScanAt = 0;
    return;
  }

  if (wasConnected && BleHidHost::hasBondedPeers() &&
      !BleHidHost::inPairingMode())
  {
    // The keyboard we were talking to just went away. Open a single
    // background-scan window. requestBackgroundScan uses 10% duty so
    // the main loop stays responsive while we wait. If the window
    // expires without a reconnect, we stay idle — user can hit
    // CALL PAIR / BOOT to re-engage.
    if (backgroundScanAt == 0)
    {
      backgroundScanAt = millis();
      BleHidHost::requestBackgroundScan(60000);
    }
    else if (!BleHidHost::inPairingMode() &&
             millis() - backgroundScanAt > 60000)
    {
      // 60-second window expired without a reconnect. Clear the
      // "had-connection" state so we don't keep retrying; user must
      // hit CALL PAIR / BOOT to force discovery again. The radio is
      // already idle (BleHidHost stops scanning when the window
      // closes); no explicit stopScan() needed here.
      wasConnected = false;
      backgroundScanAt = 0;
    }
  }
}
