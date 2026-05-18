/*
 * BleHidHost — multi-peer BLE HID host for ESP32-S3 (NimBLE stack).
 *
 * Scans for HID devices, connects to up to MAX_PEERS of them
 * simultaneously, subscribes to input reports on each, and forwards
 * raw HID reports through a single user-supplied callback. The caller
 * is expected to dispatch on report shape (length / first byte) — keep
 * the keyboard parser and the gamepad parser as separate consumers.
 *
 * Why multi-peer: in a TI BASIC simulator you need both a keyboard
 * (to type programs) AND a gamepad (CALL JOYST). A single-peer host
 * forces the user to repeatedly forget/re-pair, which is unusable.
 *
 * Why NimBLE: this file used to be Bluedroid-based, which couldn't
 * see certain BLE-HID keyboards (OMOTON KB066 observed) even with
 * passive scanning, 100% duty cycle, and Passkey Entry pairing.
 * NimBLE handles a wider variety of advertising patterns and uses
 * ~50% less flash and ~100KB less RAM. Public API is unchanged.
 *
 * Usage:
 *
 *   #include <BleHidHost.h>
 *
 *   void onHidReport(const uint8_t* data, size_t len) { ... }
 *
 *   void setup() {
 *     BleHidHost::setReportCallback(onHidReport);
 *     BleHidHost::begin("MyDeviceName");
 *   }
 *
 *   void loop() { BleHidHost::task(); }
 *
 * Pairing:
 *   BleHidHost::requestPairingMode()  — opens a 30s window during
 *     which any HID device that's NOT already paired gets adopted
 *     into the first free peer slot. Existing pairings are preserved.
 *
 *   BleHidHost::unpairAll()  — wipes all stored peers and disconnects.
 *     Use as a "factory reset" if a peer is replaced.
 *
 * State queries:
 *   BleHidHost::isConnected()   — true if ANY peer is connected
 *   BleHidHost::isReady()       — true if ANY peer has live notifications
 *   BleHidHost::inPairingMode() — true during the pairing window
 */

#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

class BleHidHost
{
public:
  // 2 = keyboard + gamepad. NimBLE on ESP32-S3 supports more central
  // connections, but 2 is what we need and keeps RAM tight.
  static const int MAX_PEERS = 2;

  typedef void (*ReportCallback)(const uint8_t* data, size_t len);

  static void setReportCallback(ReportCallback cb) { _cb = cb; }
  static void begin(const char* deviceName, const char* nvsNamespace = "blehidhost");
  static void task();
  static void enterPairingMode(uint32_t windowMs = 30000UL);
  static void unpairAll();

  // Safe to call from any context (BLE notify callbacks, ISRs, etc.).
  // The actual transition runs from task() on the main loop.
  // User-initiated pairing — opens the window AND flags it so the
  // application shows a takeover UI. For silent watchdog
  // reconnections, use requestSilentScan() instead.
  static void requestPairingMode() { _userInitiatedRequested = true; _pairingRequested = true; }
  static void requestUnpairAll()   { _unpairRequested  = true; }

  static bool isConnected();
  static bool isReady();
  static bool inPairingMode()  { return _pairingMode; }
  // True only when the user explicitly asked for pairing (BOOT
  // button / F12), not when a silent watchdog reconnect kicked off a
  // scan window. Lets the application show a full-screen pairing UI
  // for user-initiated requests while keeping background reconnects
  // invisible.
  static bool userInitiatedPairing() { return _pairingMode && _userInitiatedPairing; }
  // Request a *silent* scan window — same machinery, but the
  // userInitiatedPairing() flag stays false so the application can
  // skip any UI takeover.
  static void requestSilentScan() { _pairingRequested = true; }
  // Milliseconds remaining in the current pairing window (0 if not
  // pairing).
  static unsigned long pairingRemainingMs()
  {
    if (!_pairingMode) return 0;
    long remain = (long)(_pairingDeadline - millis());
    return remain > 0 ? (unsigned long)remain : 0;
  }
  static int  peerCount();   // number of currently-connected peers
  // True if at least one bonded peer exists in NVS, regardless of
  // whether it's currently connected. The application uses this to
  // distinguish "no keyboard ever paired" (idle, no scan) from "known
  // keyboard absent" (auto-reconnect scan).
  static bool hasBondedPeers();
  // Stop any active scan and exit pairing mode. Used by the application
  // when it wants to take the radio idle (e.g., after the title screen
  // when no keyboard ever connected, to avoid burning CPU on a scan
  // nobody is going to fulfill).
  static void stopScan();
  // Open a low-duty-cycle background scan for an auto-reconnect window.
  // Unlike requestPairingMode/requestSilentScan (which run 100/100), this
  // uses gentler parameters so the main loop stays responsive.
  static void requestBackgroundScan(uint32_t windowMs = 30000);

  // Walk the bonded-peer slot table, calling `printFn` once per slot
  // with a human-readable status line. Used by the `BLE` shell command
  // in BASIC for diagnostics ("which keyboards are paired right now?").
  typedef void (*DescribeFn)(const char* line);
  static void describePeers(DescribeFn printFn);

private:
  static NimBLEUUID _hidServiceUUID;   // 0x1812
  static NimBLEUUID _reportCharUUID;   // 0x2A4D

  struct Peer
  {
    NimBLEClient* client = nullptr;
    NimBLEAddress targetAddr;
    bool          haveTarget = false;
    volatile bool connected  = false;
    volatile bool ready      = false;
    volatile bool doConnect  = false;
    String        savedAddress;
    String        savedName;       // friendly name captured at pairing
  };
  static Peer _peers[MAX_PEERS];

  static ReportCallback _cb;
  static volatile bool _pairingMode;
  static volatile bool _pairingRequested;
  static volatile bool _userInitiatedRequested;
  static volatile bool _userInitiatedPairing;
  static volatile bool _unpairRequested;
  static unsigned long _pairingDeadline;
  static uint32_t _pairingWindowMs;
  // Number of connected peers at the moment pairing was last opened.
  // The "close-on-first-ready-peer" early-exit only fires when the
  // current peer count exceeds this — so opening pairing while a
  // device is already connected doesn't trip the exit immediately.
  static int _pairingStartPeerCount;

  static Preferences _prefs;
  static const char* _nvsNamespace;

  static const int _bootButtonPin = 0;

  // Internal helpers
  static void _notifyCb(NimBLERemoteCharacteristic* pChar,
                        uint8_t* data, size_t len, bool isNotify);
  static bool _connectToServer(int slot);
  static int  _findFreeSlot();
  static int  _findSlotByAddress(const String& addr);
  static bool _addressIsAlreadyKnownOrConnected(const String& addr);
  static void _persistPeer(int slot);
  static void _loadAllPeers();
  static void _bootButtonIsr();

  // Inner callback classes
  class ClientCallbacks;
  class ScanCallbacks;
};

// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------
inline NimBLEUUID BleHidHost::_hidServiceUUID((uint16_t)0x1812);
inline NimBLEUUID BleHidHost::_reportCharUUID((uint16_t)0x2A4D);

inline BleHidHost::Peer BleHidHost::_peers[BleHidHost::MAX_PEERS];

inline BleHidHost::ReportCallback BleHidHost::_cb = nullptr;
inline volatile bool BleHidHost::_pairingMode             = false;
inline volatile bool BleHidHost::_pairingRequested        = false;
inline volatile bool BleHidHost::_userInitiatedRequested  = false;
inline volatile bool BleHidHost::_userInitiatedPairing    = false;
inline volatile bool BleHidHost::_unpairRequested         = false;
inline unsigned long BleHidHost::_pairingDeadline         = 0;
inline uint32_t      BleHidHost::_pairingWindowMs         = 30000UL;
inline int           BleHidHost::_pairingStartPeerCount   = 0;

inline Preferences BleHidHost::_prefs;
inline const char* BleHidHost::_nvsNamespace = "blehidhost";

// ---------------------------------------------------------------------------
// Slot-management helpers
// ---------------------------------------------------------------------------
inline int BleHidHost::_findFreeSlot()
{
  // First-pass: a slot is genuinely free (no connection, no saved
  // peer).
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (!_peers[i].connected && _peers[i].savedAddress.length() == 0)
    {
      return i;
    }
  }
  // Second-pass (only meaningful in pairing mode): every slot has a
  // saved address but is not connected. The user explicitly asked to
  // pair a NEW device, so evict the first non-connected slot —
  // forgetting one stale bond to make room. Without this, two stale
  // bonds permanently lock out fresh pairing.
  if (_pairingMode)
  {
    for (int i = 0; i < MAX_PEERS; i++)
    {
      if (!_peers[i].connected)
      {
        Serial.printf("BleHidHost: evicting stale bond on slot %d (%s) "
                      "to make room\n",
                      i, _peers[i].savedAddress.c_str());
        _peers[i].savedAddress = "";
        _peers[i].ready = false;
        _prefs.begin(_nvsNamespace, false);
        char key[16];
        snprintf(key, sizeof(key), "peer_addr_%d", i);
        _prefs.remove(key);
        _prefs.end();
        return i;
      }
    }
  }
  return -1;
}

inline int BleHidHost::_findSlotByAddress(const String& addr)
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].savedAddress == addr) return i;
  }
  return -1;
}

inline bool BleHidHost::_addressIsAlreadyKnownOrConnected(const String& addr)
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].connected && _peers[i].client)
    {
      String peerAddr = _peers[i].client->getPeerAddress().toString().c_str();
      if (peerAddr == addr) return true;
    }
  }
  return false;
}

inline void BleHidHost::_persistPeer(int slot)
{
  if (slot < 0 || slot >= MAX_PEERS) return;
  _prefs.begin(_nvsNamespace, false);
  char key[16];
  snprintf(key, sizeof(key), "peer_addr_%d", slot);
  _prefs.putString(key, _peers[slot].savedAddress);
  snprintf(key, sizeof(key), "peer_name_%d", slot);
  _prefs.putString(key, _peers[slot].savedName);
  _prefs.end();
  Serial.printf("BleHidHost: slot %d saved %s '%s'\n",
                slot, _peers[slot].savedAddress.c_str(),
                _peers[slot].savedName.c_str());
}

inline void BleHidHost::_loadAllPeers()
{
  _prefs.begin(_nvsNamespace, true);
  for (int i = 0; i < MAX_PEERS; i++)
  {
    char key[16];
    snprintf(key, sizeof(key), "peer_addr_%d", i);
    _peers[i].savedAddress = _prefs.getString(key, "");
    snprintf(key, sizeof(key), "peer_name_%d", i);
    _peers[i].savedName    = _prefs.getString(key, "");
    if (_peers[i].savedAddress.length() > 0)
    {
      Serial.printf("BleHidHost: peer %d = %s '%s'\n",
                    i, _peers[i].savedAddress.c_str(),
                    _peers[i].savedName.c_str());
    }
  }
  _prefs.end();
}

// ---------------------------------------------------------------------------
// Notify callback — forwards raw HID reports to the user's handler
// ---------------------------------------------------------------------------
inline void BleHidHost::_notifyCb(NimBLERemoteCharacteristic* pChar,
                                  uint8_t* data, size_t len, bool isNotify)
{
  (void)pChar;
  (void)isNotify;
  if (_cb && data && len > 0) _cb(data, len);
}

// ---------------------------------------------------------------------------
// Inner callback classes
// ---------------------------------------------------------------------------
class BleHidHost::ClientCallbacks : public NimBLEClientCallbacks
{
  void onConnect(NimBLEClient* pClient) override
  {
    int slot = -1;
    for (int i = 0; i < MAX_PEERS; i++)
    {
      if (_peers[i].client == pClient) { slot = i; break; }
    }
    if (slot < 0) return;
    _peers[slot].connected = true;
    Serial.printf("BleHidHost: peer %d connected.\n", slot);
    // Encryption / pairing is started from _connectToServer on the
    // main loop after connect() returns. Calling NimBLE APIs from
    // inside this callback context can deadlock the host thread.
  }

  void onConnectFail(NimBLEClient* pClient, int reason) override
  {
    int slot = -1;
    for (int i = 0; i < MAX_PEERS; i++)
    {
      if (_peers[i].client == pClient) { slot = i; break; }
    }
    Serial.printf("BleHidHost: %s connect failed, reason=%d\n",
                  pClient->getPeerAddress().toString().c_str(), reason);
    if (slot >= 0)
    {
      _peers[slot].connected = false;
      _peers[slot].haveTarget = false;
    }
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override
  {
    int slot = -1;
    for (int i = 0; i < MAX_PEERS; i++)
    {
      if (_peers[i].client == pClient) { slot = i; break; }
    }
    if (slot < 0) return;
    _peers[slot].connected = false;
    _peers[slot].ready = false;
    Serial.printf("BleHidHost: peer %d disconnected (reason=%d).\n",
                  slot, reason);
  }

  // Called when the peer (e.g., an Apple keyboard with no display)
  // wants us to type a passkey it's displaying. Headless host —
  // not supported, returning won't satisfy auth.
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override
  {
    Serial.println("BleHidHost: peer requested passkey input "
                   "(host has no input — auth will likely fail).");
    NimBLEDevice::injectPassKey(connInfo, 0);
  }

  // Called when WE display a passkey for the peer to type
  // (BLE_HS_IO_DISPLAY_ONLY mode). NimBLE wants us to return the
  // 6-digit passkey that the user will type on the keyboard.
  uint32_t onPassKeyDisplay(NimBLEConnInfo& connInfo) override
  {
    (void)connInfo;
    uint32_t passkey = (uint32_t)random(100000, 1000000);
    Serial.printf("\n"
                  "==========================================\n"
                  "BleHidHost: PAIRING PASSKEY  %06lu\n"
                  "Type those 6 digits on the keyboard then\n"
                  "press Enter to complete pairing.\n"
                  "==========================================\n",
                  (unsigned long)passkey);
    return passkey;
  }

  // Numeric comparison fallback when both sides have a display.
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override
  {
    Serial.printf("BleHidHost: confirm pin %06lu (auto-accepting).\n",
                  (unsigned long)pin);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override
  {
    if (connInfo.isEncrypted())
    {
      Serial.println("BleHidHost: authentication successful.");
    }
    else
    {
      Serial.println("BleHidHost: authentication FAILED — disconnecting.");
      NimBLEClient* pClient =
        NimBLEDevice::getClientByHandle(connInfo.getConnHandle());
      if (pClient) pClient->disconnect();
    }
  }
};

class BleHidHost::ScanCallbacks : public NimBLEScanCallbacks
{
  void onResult(const NimBLEAdvertisedDevice* ad) override
  {
    if (!ad) return;
    String addr = ad->getAddress().toString().c_str();
    String name = ad->getName().c_str();
    bool isHid = ad->isAdvertisingService(_hidServiceUUID);

    // Some BLE-HID gamepads (8BitDo Zero 2, several generic pads)
    // don't include the HID Service UUID 0x1812 in their primary
    // advertisement — they put it in the GATT services after
    // connection. Use a name-substring fallback so those still
    // pair. Names checked are common gamepad/keyboard prefixes.
    bool nameLooksHid = false;
    if (name.length() > 0)
    {
      const char* hits[] = {
        "8Bit", "8bit", "Zero", "SN30", "Gamepad", "gamepad",
        "Joystick", "joystick", "Pro Controller", "DualSense",
        "DualShock", "Xbox", "Keyboard", "keyboard", "Mouse",
        // Apple keyboards: "Magic Keyboard" matches "Keyboard"
        // already, but legacy "Apple Wireless Keyboard" likewise
        // does — adding bare "Magic" / "Apple" / "iPad" / "iMac"
        // covers any short-form names we haven't seen.
        "Magic", "Apple", "iPad", "iMac",
        // Third-party BLE keyboards that advertise with a vendor /
        // model name and no HID UUID in the primary packet.
        "OMOTON", "Omoton", "KB066",
        // 8BitDo internal model codes that appear instead of the
        // friendly name on BLE: Q35 = Zero 2.
        "Q35"
      };
      for (auto h : hits)
      {
        if (name.indexOf(h) >= 0) { nameLooksHid = true; break; }
      }
    }

    // Scan-spam debug. On while we're chasing keyboards that aren't
    // being detected — flip back to #if 0 once devices are pairing
    // reliably so we don't fill the serial monitor every pairing
    // window.
#if 1
    if (_pairingMode)
    {
      int uuidCount = ad->getServiceUUIDCount();
      char uuids[120] = {0};
      int upos = 0;
      for (int i = 0; i < uuidCount && upos < (int)sizeof(uuids) - 8; i++)
      {
        std::string s = ad->getServiceUUID(i).toString();
        upos += snprintf(uuids + upos, sizeof(uuids) - upos,
                         "%s%s", upos ? "," : "", s.c_str());
      }
      uint16_t mfg = 0;
      if (ad->haveManufacturerData())
      {
        std::string md = ad->getManufacturerData();
        if (md.length() >= 2)
        {
          mfg = ((uint8_t)md[1] << 8) | (uint8_t)md[0];
        }
      }
      Serial.printf("BleHidHost: scan saw %s name='%s' hid=%d hint=%d "
                    "uuids=%d[%s] mfg=0x%04x rssi=%d\n",
                    addr.c_str(), name.c_str(),
                    isHid ? 1 : 0, nameLooksHid ? 1 : 0,
                    uuidCount, uuids, mfg, ad->getRSSI());
    }
#endif

    // Already connected as one of our peers? Ignore.
    if (_addressIsAlreadyKnownOrConnected(addr)) return;

    // Decide which slot, if any, this advertisement should bind to:
    //  1. A saved-but-not-connected peer slot whose address matches.
    //  2. (Only in pairing mode + isHid) the first totally free slot.
    int slot = _findSlotByAddress(addr);
    if (slot >= 0 && _peers[slot].connected) return;
    if (slot < 0)
    {
      if (!_pairingMode || (!isHid && !nameLooksHid)) return;
      slot = _findFreeSlot();
      if (slot < 0)
      {
        Serial.printf("BleHidHost: pairing — no free slot for %s\n",
                      addr.c_str());
        return;
      }
    }

    Serial.printf("BleHidHost: %s %s (%s) -> slot %d\n",
                  _peers[slot].savedAddress.length() ? "Reconnecting" : "Pairing",
                  ad->getName().c_str(), addr.c_str(), slot);

    NimBLEDevice::getScan()->stop();
    _peers[slot].targetAddr = ad->getAddress();
    _peers[slot].haveTarget = true;
    _peers[slot].doConnect = true;
    // Capture friendly name from the advertisement so `BLE` can show
    // something more useful than just a MAC address. Empty if the
    // device didn't advertise a name.
    _peers[slot].savedName = ad->getName().c_str();
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override
  {
    (void)results;
    (void)reason;
    // Scan ended naturally. task() will restart it on the next tick
    // if we're still in pairing mode.
  }
};

// ---------------------------------------------------------------------------
// Connect to a previously-targeted peer
// ---------------------------------------------------------------------------
inline bool BleHidHost::_connectToServer(int slot)
{
  if (slot < 0 || slot >= MAX_PEERS) return false;
  Peer& p = _peers[slot];
  if (!p.haveTarget) return false;

  Serial.printf("BleHidHost: slot %d connecting to %s...\n",
                slot, p.targetAddr.toString().c_str());

  if (!p.client)
  {
    p.client = NimBLEDevice::createClient();
    static ClientCallbacks s_cb;
    p.client->setClientCallbacks(&s_cb, false);
    p.client->setConnectionParams(12, 12, 0, 150);
    p.client->setConnectTimeout(15000);   // 15 s (units: ms — was 15 ms!)
  }

  // Connect at the GAP layer only — passing refreshServices=false
  // skips the post-connect service discovery that NimBLE does by
  // default. HID peripherals don't expose their services until
  // paired/encrypted, so service discovery here would always fail
  // and connect() would return false even though the radio is up.
  if (!p.client->connect(p.targetAddr, false /*refreshServices*/))
  {
    Serial.printf("BleHidHost: slot %d connect() returned false\n", slot);
    p.haveTarget = false;
    return false;
  }

  // Trigger pairing / encryption. Calling secureConnection from
  // outside the onConnect callback to avoid deadlocking the host
  // thread — see NimBLE_Secure_Client.ino.
  if (!p.client->secureConnection())
  {
    Serial.printf("BleHidHost: slot %d secureConnection failed\n", slot);
    p.client->disconnect();
    p.haveTarget = false;
    return false;
  }

  // Now that we're paired, discover services.
  p.client->discoverAttributes();

  // Discover HID service.
  NimBLERemoteService* pSvc = p.client->getService(_hidServiceUUID);
  if (!pSvc)
  {
    Serial.printf("BleHidHost: slot %d has no HID service — disconnecting.\n",
                  slot);
    p.client->disconnect();
    p.haveTarget = false;
    return false;
  }

  // Subscribe to every Report characteristic (UUID 0x2A4D) the
  // peripheral exposes. Some HID devices have multiple input
  // reports (keyboard + consumer keys, etc.).
  std::vector<NimBLERemoteCharacteristic*> chars = pSvc->getCharacteristics(true);
  int subscribed = 0;
  for (NimBLERemoteCharacteristic* pChar : chars)
  {
    if (pChar && pChar->getUUID() == _reportCharUUID && pChar->canNotify())
    {
      if (pChar->subscribe(true, _notifyCb))
      {
        subscribed++;
      }
    }
  }
  Serial.printf("BleHidHost: slot %d ready, %d input report(s).\n",
                slot, subscribed);

  p.ready = true;
  // Persist the address (use the canonical lowercase form NimBLE returns).
  p.savedAddress = p.targetAddr.toString().c_str();
  _persistPeer(slot);
  p.haveTarget = false;
  return true;
}

// ---------------------------------------------------------------------------
// Public state queries
// ---------------------------------------------------------------------------
inline bool BleHidHost::isConnected()
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].connected) return true;
  }
  return false;
}

inline bool BleHidHost::isReady()
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].connected && _peers[i].ready) return true;
  }
  return false;
}

inline int BleHidHost::peerCount()
{
  int n = 0;
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].connected) n++;
  }
  return n;
}

inline void BleHidHost::describePeers(DescribeFn printFn)
{
  if (!printFn) return;
  char line[96];
  printFn("BLE peers:");
  bool any = false;
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].savedAddress.length() == 0 && !_peers[i].connected)
    {
      continue;
    }
    any = true;
    const char* state = _peers[i].connected
                          ? (_peers[i].ready ? "CONNECTED" : "CONNECTING")
                          : "BONDED";
    const char* mac = _peers[i].savedAddress.length()
                        ? _peers[i].savedAddress.c_str()
                        : "(no NVS bond)";
    const char* name = _peers[i].savedName.length()
                         ? _peers[i].savedName.c_str()
                         : "(no name)";
    snprintf(line, sizeof(line), "  slot %d: %-20s %-18s %s",
             i, name, mac, state);
    printFn(line);
  }
  if (!any) printFn("  (none)");
  snprintf(line, sizeof(line), "Pairing mode: %s",
           _pairingMode ? "OPEN" : "closed");
  printFn(line);
}

inline bool BleHidHost::hasBondedPeers()
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].savedAddress.length() > 0) return true;
  }
  return false;
}

inline void BleHidHost::stopScan()
{
  // Take the radio idle. Used when the application decides nobody is
  // expected to connect (e.g., after the title screen with no bonded
  // peers). Clears every state flag so a pending requestSilentScan() /
  // requestPairingMode() that hadn't been consumed by task() yet can't
  // re-arm scanning on the next tick.
  _pairingMode = false;
  _userInitiatedPairing = false;
  _pairingRequested = false;
  _userInitiatedRequested = false;
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan) pScan->stop();
}

inline void BleHidHost::requestBackgroundScan(uint32_t windowMs)
{
  // Low-duty-cycle scan to reacquire a bonded peer without hammering
  // the radio. 50 ms window / 500 ms interval = 10% duty cycle — enough
  // to spot the keyboard's wake-up advertising within a few seconds
  // while leaving 90% of the radio's time available for everything else.
  Serial.printf("BleHidHost: background reconnect scan (%lus)\n",
                (unsigned long)(windowMs / 1000));
  _pairingMode = true;        // reuses the pairing-mode window machinery
  _userInitiatedPairing = false;
  _pairingWindowMs = windowMs;
  _pairingDeadline = millis() + windowMs;
  _pairingStartPeerCount = peerCount();

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->stop();
  pScan->clearResults();
  pScan->setInterval(500);   // 500 ms
  pScan->setWindow(50);      // 50 ms — 10% duty
  pScan->start(windowMs, false);
}

// ---------------------------------------------------------------------------
// Setup / lifecycle
// ---------------------------------------------------------------------------
inline void BleHidHost::begin(const char* deviceName, const char* nvsNamespace)
{
  _nvsNamespace = nvsNamespace;
  _loadAllPeers();

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setPower(3);   // +3 dBm

  // Bonding + MITM protection. Secure Connections (SC) on. With SC
  // and DISPLAY_ONLY, peripherals that demand passkey entry get our
  // 6-digit code via onPassKeyDisplay; Just-Works peers skip it.
  NimBLEDevice::setSecurityAuth(true /*bond*/, true /*mitm*/, true /*sc*/);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  static ScanCallbacks s_scanCb;
  pScan->setScanCallbacks(&s_scanCb, false);
  pScan->setInterval(100);   // 100 ms
  pScan->setWindow(100);     // 100 ms (100% duty cycle)
  pScan->setActiveScan(true);
  // Start a 5-second initial scan to give saved peers a chance to
  // be re-discovered. After this, scans only run during pairing
  // windows (see task()).
  pScan->start(5 * 1000, false);

  pinMode(_bootButtonPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(_bootButtonPin),
                  _bootButtonIsr, FALLING);
  Serial.println("BleHidHost: scanning...");
}

inline void BleHidHost::enterPairingMode(uint32_t windowMs)
{
  Serial.printf("BleHidHost: entering pairing mode (%lus). "
                "Existing peers preserved.\n", (unsigned long)(windowMs / 1000));
  _pairingMode = true;
  _pairingWindowMs = windowMs;
  _pairingDeadline = millis() + windowMs;
  _pairingStartPeerCount = peerCount();

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->stop();
  pScan->clearResults();
  // Aggressive scan for active pairing — user is waiting, find devices
  // fast. requestBackgroundScan() uses gentler params for reconnect.
  pScan->setInterval(100);
  pScan->setWindow(100);
  pScan->start(0, false);   // 0 = scan until stopped
}

inline void BleHidHost::unpairAll()
{
  Serial.println("BleHidHost: forgetting all peers.");
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].client && _peers[i].client->isConnected())
    {
      _peers[i].client->disconnect();
    }
    _peers[i].savedAddress = "";
    _peers[i].connected = false;
    _peers[i].ready = false;
    _peers[i].haveTarget = false;
  }
  _prefs.begin(_nvsNamespace, false);
  for (int i = 0; i < MAX_PEERS; i++)
  {
    char key[16];
    snprintf(key, sizeof(key), "peer_addr_%d", i);
    _prefs.remove(key);
  }
  _prefs.end();
  // Wipe NimBLE's bond table too, so the host doesn't try to
  // re-encrypt with stale keys.
  NimBLEDevice::deleteAllBonds();
}

// ---------------------------------------------------------------------------
// Per-loop maintenance task
// ---------------------------------------------------------------------------
inline void BleHidHost::task()
{
  if (_pairingRequested)
  {
    _pairingRequested = false;
    _userInitiatedPairing = _userInitiatedRequested;
    _userInitiatedRequested = false;
    enterPairingMode(_pairingWindowMs);
  }
  if (_unpairRequested)
  {
    _unpairRequested = false;
    unpairAll();
  }

  // Process any pending connect requests.
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].doConnect)
    {
      _peers[i].doConnect = false;
      _connectToServer(i);
    }
  }

  if (_pairingMode && (long)(millis() - _pairingDeadline) >= 0)
  {
    Serial.println("BleHidHost: pairing window expired.");
    _pairingMode = false;
    _userInitiatedPairing = false;
    NimBLEDevice::getScan()->stop();
  }

  // Close pairing mode early once a NEW peer has paired and become
  // ready (the count of connected peers exceeds what we had when the
  // window opened). The new-vs-existing distinction is important —
  // opening pairing for a second device while one is already
  // connected used to insta-close because the existing device was
  // already in the connected+ready state.
  if (_pairingMode && peerCount() > _pairingStartPeerCount)
  {
    for (int i = 0; i < MAX_PEERS; i++)
    {
      if (_peers[i].connected && _peers[i].ready)
      {
        Serial.println("BleHidHost: pairing complete; closing window.");
        _pairingMode = false;
        _userInitiatedPairing = false;
        NimBLEDevice::getScan()->stop();
        break;
      }
    }
  }
}

// ISR for the BOOT button. Just flag the request — actual BLE work
// happens on the next task() call. Not IRAM_ATTR because the volatile
// flag we touch lives in flash; the linker rejects mixed IRAM/flash
// access. Acceptable since BOOT-press timing isn't critical and we
// won't be in the middle of a flash write when the user prods it.
inline void BleHidHost::_bootButtonIsr()
{
  // Idempotent flag set; bounce-safe because re-asserting it has no
  // effect.
  _pairingRequested = true;
}
