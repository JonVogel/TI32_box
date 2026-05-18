// ti_platform.h
//
// Platform-glue hooks for the TI Extended BASIC interpreter.
//
// This header declares every function that bridges the language layer
// (which lives in this library) to a project's hardware: character /
// screen rendering, sprite draw, sound, and joystick input.
//
// Each hook has a weak no-op default in ti_platform.cpp. To override
// from a project, define a strong (non-weak) function with the same
// signature in your .ino — the linker prefers strong over weak so your
// version wins.
//
//   void tiPrintChar(char c) { Serial.write(c); ... }   // strong override
//
// Hooks deliberately NOT in here (and that DO still use explicit
// setCallbacks() on TokenParser): file I/O (FLASH/SD/DSK<n>), shell
// commands (NEW/RUN/SAVE/OLD/etc.), throttle, IMAGE lookup, DATA/READ.
// Those are language-layer concerns, not hardware.

#ifndef TI_PLATFORM_H
#define TI_PLATFORM_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Cooperative yield — every long-running loop in the interpreter, file
// I/O, and graphics layers calls this so the host OS can schedule its
// other tasks (FreeRTOS IDLE, BLE host, WiFi, etc.).
//
// IMPORTANT: this must do MORE than Arduino's yield() / FreeRTOS
// taskYIELD(), which only switch among same-or-higher-priority tasks.
// To let the lowest-priority IDLE task run (so it can reset the task
// watchdog), the host's strong override should block the caller for
// at least one tick — e.g. `delay(1)` on Arduino, vTaskDelay(1) on
// raw FreeRTOS. Default weak: no-op.
// ---------------------------------------------------------------------------
void tiYield();

// ---------------------------------------------------------------------------
// Console output
// ---------------------------------------------------------------------------
void tiPrintChar(char c);
void tiPrintString(const char* s);
void tiClearScreen();

// ---------------------------------------------------------------------------
// Char grid manipulation (CALL HCHAR / VCHAR / GCHAR / CHAR / COLOR / SCREEN)
// ---------------------------------------------------------------------------
void tiSetChar(int row, int col, char ch);
char tiGetChar(int row, int col);
void tiSetScreenColor(int idx);                       // CALL SCREEN(idx)
void tiSetCharColor(int set, int fg, int bg);         // CALL COLOR
void tiSetCharPattern(int charCode, const uint8_t* bytes);   // CALL CHAR
void tiGetCharPattern(int charCode, uint8_t* bytes);
void tiResetCharset();
// CALL CHARSET("PC" | "TI") — switch the default font ROM the simulator
// uses for chars 32-127. Host should persist the selection (e.g. NVS) and
// refresh `charPatterns[]` before the next CALL CHARSET (which actually
// reloads 32-127). Mode values match the TiFontMode enum in ti_font.h.
void tiSetCharsetMode(int mode);
void tiMoveCursor(int row, int col);                  // DISPLAY AT

// ---------------------------------------------------------------------------
// Sprites (CALL SPRITE / PATTERN / LOCATE / DELSPRITE / MAGNIFY)
// ---------------------------------------------------------------------------
void tiSpriteDraw(int slot);
void tiSpriteErase(int slot);

// ---------------------------------------------------------------------------
// Input — CALL KEY / CALL JOYST
// ---------------------------------------------------------------------------
int  tiReadKey();                                     // -1 == no key
void tiReadJoystick(int unit, int* outX, int* outY);  // each in {-4, 0, +4}

// ---------------------------------------------------------------------------
// BLE pairing — CALL PAIR / CALL UNPAIR
// ---------------------------------------------------------------------------
// CALL PAIR — open a 30-second pairing window so a new BLE HID device
// can join. Default no-op; projects with a BLE HID host override this
// with their own pairing trigger (e.g., BleHidHost::requestPairingMode).
void tiPair();
// CALL UNPAIR — forget every currently-bonded BLE HID peer. Default
// no-op; the host project routes this to its own NVS-clearing path.
void tiUnpair();

// ---------------------------------------------------------------------------
// Sound — CALL SOUND
// ---------------------------------------------------------------------------
// CALL SOUND(duration, f1, v1 [, f2, v2 [, f3, v3 [, f4, v4]]]).
// Voices 1..3 are tone, voice 4 is noise.
//   duration: milliseconds. Positive = wait for prior sound to finish
//             before starting; negative = cancel prior, start immediately.
//             Range -4250..+4250.
//   freq:     for tones, Hz (110..40000). For noise voice, -1..-8 TI
//             noise type. 0 means voice unused.
//   vol:      0 (loudest) .. 30 (silent), SN76489 convention.
//
// The interpreter handles the wait-for-previous and cancel-in-flight
// timing. The platform's tiSoundPlay just programs the chip. The
// platform is responsible for stopping voices when their duration
// elapses (or it can ignore duration and rely on tiSoundStop being
// called at the right time).
void tiSoundPlay(int duration,
                 int freq1, int vol1,
                 int freq2, int vol2,
                 int freq3, int vol3,
                 int freq4, int vol4);
void tiSoundStop();

// ---------------------------------------------------------------------------
// Speech — CALL SAY / CALL SPGET
// ---------------------------------------------------------------------------
// CALL SAY [(wordStr [, phraseStr])]
//   wordStr   : space-separated vocabulary word names ("HELLO TEXAS"),
//               or NULL when only a phrase is supplied.
//   phraseBytes/phraseLen : pre-fetched LPC byte string from CALL SPGET,
//                           or NULL/0 when speaking words only.
// Both may be supplied together; the words speak first, then the phrase.
// Default weak: no-op. The host overrides with a real TMS5220 synth
// driving the existing I²S audio mixer (PolyBLEP / SN76489 path).
void tiSay(const char* wordStr,
           const uint8_t* phraseBytes, int phraseLen);

// CALL SPGET(word$, phrase$)
//   word     : ASCII word to look up in the speech ROM vocabulary.
//   outBuf   : caller-provided buffer to receive the LPC byte sequence.
//   bufSize  : capacity of outBuf in bytes.
// Returns the number of bytes written (0 .. bufSize), or 0 if the word
// is not in the vocabulary. Real TI speech phrases run 30-200 bytes;
// a 256-byte buffer is plenty.
int  tiSpget(const char* word, uint8_t* outBuf, int bufSize);

// ---------------------------------------------------------------------------
// WiFi — CALL WIFI
// ---------------------------------------------------------------------------
// CALL WIFI                            — print current status
// CALL WIFI(ssid$, pass$)              — set credentials, persist, connect
// CALL WIFI("forget")                  — clear credentials, disconnect
//
// Non-TI extension. Defaults are weak no-ops; the host project routes
// these to its WiFi manager (e.g., the ESP32-S3 STA stack with NVS-backed
// credentials).
//
// tiWifiStatus fills `out` with a one-line summary suitable for printing
// at the BASIC prompt. Format is host-defined; default puts "OFFLINE".
void tiWifiSet(const char* ssid, const char* pass);
void tiWifiForget();
void tiWifiStatus(char* out, int outSize);
// Toggle the radio without losing stored credentials.
//   tiWifiOff(): disconnect + WiFi.mode(WIFI_OFF). Stops the HTTP server
//                and prevents reconnect-induced flash writes / display
//                glitches while still leaving creds in NVS for later.
//   tiWifiOn():  re-enable radio and reconnect using stored creds.
void tiWifiOff();
void tiWifiOn();

#endif // TI_PLATFORM_H
