# BleHidHost

Multi-peer BLE HID host for ESP32-S3 (NimBLE stack). Connects to up to two HID devices (keyboard + gamepad), subscribes to their input reports, and forwards reports to a user callback. Persists bonded peers in NVS for fast reconnect.

## Features

- NimBLE stack — broader keyboard compatibility than Bluedroid (~50% less flash, ~100 KB less RAM).
- 30-second pairing window triggered by the caller (BOOT button, hotkey, watchdog).
- Two scan modes: user-initiated (UI takeover OK) and silent (background reconnect during a running BASIC program).
- BOOT-button ISR for instant pairing without polling.
- Stale-bond eviction so a full slot table doesn't lock out a new pair attempt.

## Public API

```cpp
BleHidHost::begin();
BleHidHost::setReportCallback(myCb);
BleHidHost::loop();                       // call from your loop()

BleHidHost::requestPairingMode();         // 30s window with user-initiated UI flag
BleHidHost::requestSilentScan();          // same scan, no UI takeover (sleeping-keyboard reconnect)
BleHidHost::requestUnpairAll();
bool inPairing = BleHidHost::inPairingMode();
bool userInitiated = BleHidHost::userInitiatedPairing();
unsigned long ms = BleHidHost::pairingRemainingMs();
int n = BleHidHost::peerCount();
```

## Used by

- esp32-s3-box-basic
- ti-extended-basic-esp32
- ti-basic-otg
- ti-scott-adams-esp32

## Embedding

```sh
git submodule add https://github.com/JonVogel/BleHidHost.git
```

Requires the `NimBLE-Arduino` library (>= 2.x) installed via `arduino-cli lib install NimBLE-Arduino`.
