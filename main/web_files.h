// WiFi + HTTP file manager for the ESP32-8048S043C TI Extended BASIC port.
//
// Step 1 (this file's current scope): bring WiFi up in STA mode from
// credentials in NVS, run an async HTTP server on port 80, expose a
// single read-only /api/status endpoint. No file operations yet —
// upload / download / delete / rename land in later steps.
//
// User-facing entry points (called from CALL WIFI in token_parser via
// strong-override tiWifi* hooks in the .ino):
//   webBegin()                 - read NVS, start WiFi + server if creds.
//   webSetCredentials(s, p)    - store ssid/pass in NVS, reconnect.
//   webForget()                - clear NVS, disconnect, stop server.
//   webStatus(line, lineSize)  - one-line summary printed by CALL WIFI.
//   webSetBusy(bool)           - exec_manager tells us when RUN is
//                                active so future write endpoints can
//                                return 503. Read by /api/status today
//                                so the front-end can show a banner.
//
// NVS layout (Preferences namespace "wifi"):
//   "ssid"  - String, the SSID to join
//   "pass"  - String, WPA2 passphrase
//
// Bootstrap: a board with no NVS creds boots with WiFi off. The user
// types CALL WIFI("ssid","pass") once at the serial prompt; the
// credentials are persisted and the board reconnects on every future
// boot until CALL WIFI("forget") is run.

#pragma once

#include <Arduino.h>

namespace webfiles
{
  void begin();
  void tick();   // called from main loop; lets us print "connected" on edge

  // Returns true if credentials were stored. Triggers a reconnect.
  bool setCredentials(const char* ssid, const char* pass);

  // Turn the radio off without losing stored creds. Use for display-
  // critical work where WiFi reconnects must not glitch the panel.
  void radioOff();

  // Turn the radio on and reconnect using stored creds (no-op if no
  // creds in NVS).
  void radioOn();

  // Clear NVS, disconnect, stop the server.
  void forget();

  // Fill `out` with a single human-readable status line. Used by
  // CALL WIFI to print the current state at the BASIC prompt.
  // Format: "SSID  IP=x.x.x.x  RSSI=-NNdBm  ONLINE" / "OFFLINE".
  void status(char* out, int outSize);

  // exec_manager calls this with true when RUN starts and false when
  // the program ends (END / STOP / BREAK / error). When busy, future
  // write endpoints will return 503; today /api/status reports it.
  void setBusy(bool busy);
}
