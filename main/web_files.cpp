// See web_files.h for design notes.

#include "web_files.h"

// flash_trace.h lived in the sibling ti-extended-basic-esp32 project for
// correlating display tearing with NVS write events on the 8048S043C's
// RGB panel. The Box-3 has no such problem (SPI display, not RGB), so
// flash tracing isn't needed here — stub the three macros out so the
// call sites compile unchanged. If we ever need flash timing data on
// Box-3, restore flash_trace.h from the sibling project.
#define FLASH_TRACE_START(tag) ((void)0)
#define FLASH_TRACE_END(tag)   ((void)0)
#define FLASH_TRACE_MARK(tag)  ((void)0)

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include "file_io.h"
#include "ti_platform.h"   // tiYield()

namespace webfiles
{
  // State.
  static AsyncWebServer s_server(80);
  static bool s_serverRunning = false;
  static bool s_announcedConnected = false;   // edge-print "WiFi: IP=..."
  static bool s_busy = false;                  // RUN active?

  static const char* NVS_NAMESPACE = "wifi";

  // Read SSID + pass from NVS into the given buffers. Returns true if
  // a non-empty SSID was found. Pass buffers may be small; long
  // passphrases get truncated but that's an unrealistic configuration
  // (max is 63 chars per WPA2).
  static bool readCreds(char* ssid, int ssidSize,
                        char* pass, int passSize)
  {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, /*readonly=*/true))
    {
      return false;
    }
    size_t sLen = p.getString("ssid", ssid, ssidSize);
    size_t pLen = p.getString("pass", pass, passSize);
    p.end();
    (void)pLen;
    return sLen > 0 && ssid[0] != '\0';
  }

  static void startServerOnce()
  {
    if (s_serverRunning) return;

    // /api/status — small JSON used by the front-end to know whether
    // the board is currently running a BASIC program (so it can
    // disable uploads in the UI). Also useful as a "is the board
    // alive?" ping.
    s_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
      char body[160];
      snprintf(body, sizeof(body),
               "{\"busy\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
               s_busy ? "true" : "false",
               WiFi.SSID().c_str(),
               WiFi.localIP().toString().c_str(),
               (int)WiFi.RSSI());
      req->send(200, "application/json", body);
    });

    // /api/files?dev=FLASH | SDCARD | DSK1..DSK9 / DSKA..DSKZ
    // Returns JSON:
    //   {"device":"FLASH",
    //    "volume":{"name":"FLASH","total":1572864,"free":1023488},
    //    "files":[{"name":"X","size":N},...]}
    // or {"device":"...","error":"..."} on a problem.
    // /api/devices — enumerate currently-available devices for the
    // device-picker dropdown. FLASH is always present (it's the
    // firmware's own FS), SDCARD only if SD is mounted, and DSK<n>
    // entries only for mounted slots. Each entry returns:
    //   {"id":"DSK1","label":"DSK1 (WIFIDSK)"}
    // so the dropdown can show the volume name where applicable.
    s_server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest* req) {
      String body;
      body.reserve(512);
      body += "{\"devices\":[";
      bool first = true;
      auto addDev = [&](const char* id, const char* label) {
        if (!first) body += ',';
        first = false;
        body += "{\"id\":\"";
        body += id;
        body += "\",\"label\":\"";
        for (const char* p = label; *p; ++p)
        {
          if (*p == '"' || *p == '\\') body += '\\';
          body += *p;
        }
        body += "\"}";
      };

      addDev("FLASH", "FLASH");
      // SDCARD intentionally NOT advertised in this build — isolating
      // the SD path for diagnostic purposes. If the web stays solid
      // without SD, we know the SD touch (open("/"), readSector, etc.)
      // is what's wedging AsyncTCP. Re-enable when we have a safer
      // strategy (off-task SD probing, or sdkconfig DRAM headroom).

      // Mounted DSK<n> drives. drive index 1..MAX_DSK maps to
      // DSK1..DSK9 / DSKA..DSKZ via driveToChar().
      for (int d = 1; d <= fio::MAX_DSK; d++)
      {
        if (!fio::g_mounts[d].mounted) continue;
        char id[8];
        snprintf(id, sizeof(id), "DSK%c", fio::driveToChar(d));
        // Best-effort volume-name suffix; falls back to bare id if the
        // image isn't readable for some reason.
        dsk::DskImage* img = fio::dskImage(d);
        char label[32];
        if (img)
        {
          char vn[11];
          strncpy(vn, img->vib().name, 10);
          vn[10] = '\0';
          for (int j = 9; j >= 0 && vn[j] == ' '; j--) vn[j] = '\0';
          if (vn[0]) snprintf(label, sizeof(label), "%s (%s)", id, vn);
          else       snprintf(label, sizeof(label), "%s", id);
        }
        else
        {
          snprintf(label, sizeof(label), "%s", id);
        }
        addDev(id, label);
      }
      body += "]}";
      req->send(200, "application/json", body);
    });

    s_server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest* req) {
      String dev = req->hasParam("dev") ? req->getParam("dev")->value() : "FLASH";

      // Volume header fields filled per-device below, then emitted
      // before the files array. Sizes are always reported in BYTES so
      // the JS just divides by 1024 to display.
      String volName;
      uint64_t volTotal = 0;
      uint64_t volFree  = 0;

      String body;
      body.reserve(2048);
      body += "{\"device\":\"";
      body += dev;
      body += "\",\"files\":[";
      bool first = true;
      auto appendFile = [&](const char* name, long size) {
        if (!first) body += ',';
        first = false;
        body += "{\"name\":\"";
        // Bare minimum escaping for JSON. File systems shouldn't normally
        // produce names with quotes or backslashes but be defensive.
        for (const char* p = name; *p; ++p)
        {
          if (*p == '"' || *p == '\\') body += '\\';
          body += *p;
        }
        body += "\",\"size\":";
        body += String(size);
        body += '}';
      };

      bool ok = true;
      String error;

      // CRITICAL: each File from openNextFile must be closed before the
      // next call. The SD_MMC FAT layer has a 5-slot handle pool (default
      // maxOpenFiles=5), and leaking handles exhausts it after ~5 entries —
      // subsequent SD opens then silently fail. LittleFS is more forgiving
      // but the same pattern applies for consistency.
      if (dev.equalsIgnoreCase("FLASH"))
      {
        volName  = "FLASH";
        volTotal = (uint64_t)LittleFS.totalBytes();
        volFree  = volTotal - (uint64_t)LittleFS.usedBytes();

        File root = LittleFS.open("/");
        File f = root.openNextFile();
        while (f)
        {
          const char* n = f.name();
          bool hide = f.isDirectory() || n[0] == '.';
          if (!hide) appendFile(n, (long)f.size());
          f.close();
          f = root.openNextFile();
        }
        root.close();
      }
      else if (dev.equalsIgnoreCase("SDCARD") || dev.equalsIgnoreCase("SD"))
      {
        // Temporarily disabled for diagnostic isolation. If the web
        // server stays solid without ever touching SD, we know the
        // SD enumeration path is what's wedging AsyncTCP.
        ok = false;
        error = "SDCARD disabled in this build (diagnostic)";
        if (false)
        {
          volName  = "SDCARD";
          volTotal = SD_MMC.totalBytes();
          volFree  = volTotal - SD_MMC.usedBytes();

          File root = SD_MMC.open("/");
          if (!root)
          {
            // totalBytes()/usedBytes() are cached from mount; open("/")
            // drives a fresh sector read that fails on out-of-mem.
            // Invalidate so the NEXT request retries from a clean
            // slate rather than reporting an empty directory.
            fio::notifySDFailure();
            ok = false;
            error = "SD read failed (low memory? try CALL WIFI(\"off\"))";
          }
          else
          {
            int seen = 0;
            File f = root.openNextFile();
            while (f)
            {
              const char* n = f.name();
              bool hide = f.isDirectory() || n[0] == '.' ||
                          strcasecmp(n, "System Volume Information") == 0;
              if (!hide) appendFile(n, (long)f.size());
              seen++;
              f.close();
              f = root.openNextFile();
            }
            root.close();
            // Diagnostic: if the cached used-bytes says the card has
            // real content but we enumerated zero entries, the dir
            // read silently failed (sdmmc out-of-mem returns an empty
            // openNextFile chain). Surface it instead of claiming the
            // card is empty.
            if (seen == 0 && volTotal > 0 &&
                (volTotal - volFree) > 64ULL * 1024ULL)
            {
              fio::notifySDFailure();
              ok = false;
              error = "SD read failed (low memory? try CALL WIFI(\"off\"))";
            }
          }
        }
      }
      else if (dev.length() >= 4 &&
               (dev[0] == 'd' || dev[0] == 'D') &&
               (dev[1] == 's' || dev[1] == 'S') &&
               (dev[2] == 'k' || dev[2] == 'K'))
      {
        int drive = fio::driveFromChar(dev[3]);
        dsk::DskImage* img = (drive > 0) ? fio::dskImage(drive) : nullptr;
        if (!img) { ok = false; error = "not mounted"; }
        else
        {
          // Pull volume name + sizes from the VIB. The 10-char volume
          // name is space-padded in the on-disk format; trim trailing
          // spaces so the UI shows "WORK" not "WORK      ".
          const dsk::Vib& v = img->vib();
          char vn[11];
          strncpy(vn, v.name, 10);
          vn[10] = '\0';
          for (int j = 9; j >= 0 && vn[j] == ' '; j--) vn[j] = '\0';
          volName  = vn;
          volTotal = (uint64_t)v.totalSectors * 256ULL;
          volFree  = (uint64_t)img->freeSectors() * 256ULL;

          dsk::DskImage::CatEntry ents[64];
          int n = img->listCatalog(ents, 64);
          for (int i = 0; i < n; i++)
          {
            // DSK catalog stores filenames as 10-char fixed; trim trailing
            // spaces for nicer JSON. Size is sector count (256 B each).
            char nm[12];
            strncpy(nm, ents[i].name, 10);
            nm[10] = '\0';
            for (int j = 9; j >= 0 && nm[j] == ' '; j--) nm[j] = '\0';
            appendFile(nm, (long)ents[i].totalSectors * 256L);
          }
        }
      }
      else
      {
        ok = false;
        error = "unknown device";
      }

      if (!ok)
      {
        body = "{\"device\":\"";
        body += dev;
        body += "\",\"error\":\"";
        body += error;
        body += "\"}";
      }
      else
      {
        // Close the files array and append the volume object after it.
        // Order doesn't matter for JSON consumers; placing it last keeps
        // the streaming-append in the file loop above untouched.
        body += "],\"volume\":{\"name\":\"";
        for (const char* p = volName.c_str(); *p; ++p)
        {
          if (*p == '"' || *p == '\\') body += '\\';
          body += *p;
        }
        // Emit as 64-bit literals via snprintf so SD cards > 4 GB don't
        // wrap. String(uint32_t) would truncate a 64 GB card's total
        // bytes silently.
        char numbuf[32];
        body += "\",\"total\":";
        snprintf(numbuf, sizeof(numbuf), "%llu", (unsigned long long)volTotal);
        body += numbuf;
        body += ",\"free\":";
        snprintf(numbuf, sizeof(numbuf), "%llu", (unsigned long long)volFree);
        body += numbuf;
        body += "}}";
      }
      req->send(200, "application/json", body);
    });

    // Root HTML page. Plain, server-rendered shell that uses fetch() to
    // populate the file table from /api/files. No JS framework, no
    // external CDN — the whole UI is self-contained and works without
    // internet access from the browser (only LAN access to the device).
    s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
      // Listing all common devices up front so the dropdown is populated
      // statically. The user changes the device and the JS refetches.
      // Kept short and inline so the entire UI lives in this one file.
      static const char PAGE[] PROGMEM =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<title>TI Extended BASIC Files</title>"
        "<style>"
        "body{font-family:monospace;background:#001;color:#cdf;margin:1em;}"
        "h1{color:#fff;border-bottom:1px solid #468;padding-bottom:.3em;}"
        "select,button{font-family:monospace;background:#024;color:#cdf;"
        "border:1px solid #468;padding:.3em .6em;}"
        "table{margin-top:1em;border-collapse:collapse;}"
        "th,td{padding:.2em .8em;text-align:left;border-bottom:1px solid #246;}"
        "th{color:#fff;}"
        "tr:hover{background:#012;}"
        ".err{color:#f88;}"
        ".muted{color:#789;font-size:.9em;}"
        "</style></head><body>"
        "<h1>TI Extended BASIC &mdash; File Browser</h1>"
        "<label>Device: <select id=dev></select></label> "
        "<button onclick=refreshAll()>Refresh</button>"
        "<div class=muted id=volinfo></div>"
        "<span class=muted id=status></span>"
        "<table id=ftbl><thead><tr><th>Name</th><th>Size</th></tr>"
        "</thead><tbody></tbody></table>"
        "<script>"
        "function fmt(n){"  // human-readable byte sizes
        "if(n<1024)return n+' B';"
        "if(n<1048576)return (n/1024).toFixed(1)+' KB';"
        "if(n<1073741824)return (n/1048576).toFixed(1)+' MB';"
        "return (n/1073741824).toFixed(2)+' GB';}"
        "async function refresh(){"
        "const d=document.getElementById('dev').value;"
        "const s=document.getElementById('status');"
        "const vi=document.getElementById('volinfo');"
        "const tb=document.querySelector('#ftbl tbody');"
        "tb.innerHTML='';vi.textContent='';s.textContent='loading...';"
        "try{"
        "const r=await fetch('/api/files?dev='+encodeURIComponent(d));"
        "const j=await r.json();"
        "if(j.error){s.innerHTML='<span class=err>'+j.error+'</span>';return;}"
        "if(j.volume){"
        "const used=j.volume.total-j.volume.free;"
        "vi.textContent='Volume: '+j.volume.name+"
        "' — '+fmt(used)+' used of '+fmt(j.volume.total)+"
        "' ('+fmt(j.volume.free)+' free)';}"
        "s.textContent=j.files.length+' file(s)';"
        "for(const f of j.files){"
        "const tr=document.createElement('tr');"
        "tr.innerHTML='<td>'+f.name+'</td><td>'+fmt(f.size)+'</td>';"
        "tb.appendChild(tr);"
        "}}catch(e){s.innerHTML='<span class=err>'+e+'</span>';}}"
        // Populate the device dropdown from /api/devices on load. The
        // Refresh button re-runs both — handy after mounting a DSK at
        // the BASIC prompt so it appears in the dropdown without
        // reloading the page.
        "async function loadDevices(){"
        "const sel=document.getElementById('dev');"
        "const prev=sel.value;"
        "try{"
        "const r=await fetch('/api/devices');"
        "const j=await r.json();"
        "sel.innerHTML='';"
        "for(const d of j.devices){"
        "const o=document.createElement('option');"
        "o.value=d.id;o.textContent=d.label;sel.appendChild(o);}"
        "if(prev){"
        "for(const o of sel.options){if(o.value===prev){sel.value=prev;break;}}}"
        "}catch(e){"
        "document.getElementById('status').innerHTML="
        "'<span class=err>devices: '+e+'</span>';}}"
        // refreshAll re-runs both endpoints; Refresh button uses this
        // so a DSK mounted at the BASIC prompt becomes visible in the
        // dropdown without a full page reload.
        "async function refreshAll(){await loadDevices();await refresh();}"
        "document.getElementById('dev').onchange=refresh;"
        "refreshAll();"
        "</script></body></html>";
      // Send the page as a String. Verified working via curl: the
      // 2.5 KB HTML/JS payload arrives intact in one HTTP/1.1 200
      // response. send_P / chunked-callback both stalled earlier
      // for reasons not fully understood; this is the known-good
      // path on this fork.
      req->send(200, "text/html", String(PAGE));
    });

    // 404 for anything else. Endpoints handled above: GET /, /api/status,
    // /api/files. Future endpoints (upload, download, delete) hang off
    // /api/files/... so they shadow this.
    s_server.onNotFound([](AsyncWebServerRequest* req) {
      req->send(404, "text/plain", "not found\n");
    });

    s_server.begin();
    s_serverRunning = true;
    Serial.println("webfiles: HTTP server listening on :80");
  }

  // Bring WiFi up in STA mode with the given creds. Non-blocking —
  // tick() will notice the connection and print the IP when it lands.
  //
  // Coexistence tuning for the Sunton 8048S043C: WiFi and the RGB
  // panel both contend for the PSRAM bus, and WiFi bursts can starve
  // the bounce-buffer refill ISR and visibly tear the display. The
  // settings below keep the radio as quiet as we can while remaining
  // online:
  //   - WIFI_PS_MAX_MODEM: longer sleep windows than MIN_MODEM (~200ms
  //     vs ~70ms). Trade response latency for fewer radio wakeups.
  //   - setTxPower(WIFI_POWER_8_5dBm): half the TX duration of the
  //     19.5 dBm default. Range cost is fine for LAN file transfer.
  static void connectWith(const char* ssid, const char* pass)
  {
    Serial.printf("webfiles: connecting to '%s'\n", ssid);
    FLASH_TRACE_START("wifi-begin");
    if (WiFi.getMode() == WIFI_OFF)
    {
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(WIFI_PS_MAX_MODEM);
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
    }
    else
    {
      WiFi.disconnect(false /*wifi_off*/, false /*erase config*/);
    }
    WiFi.begin(ssid, pass);
    s_announcedConnected = false;
    FLASH_TRACE_END("wifi-begin");
  }

  void begin()
  {
    char ssid[40] = {0};
    char pass[72] = {0};
    if (!readCreds(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
      Serial.println("webfiles: no WiFi credentials in NVS. "
                     "Type CALL WIFI(\"ssid\",\"pass\") to set them.");
      return;
    }
    connectWith(ssid, pass);
  }

  void tick()
  {
    // Cooperative yield. We're called from many long-running BASIC
    // command paths (catPrintLine -> per-file DIR output, etc.) and
    // need to let the scheduler run. The actual yield mechanism is
    // owned by the host (ti_platform.cpp / main.cpp's strong override).
    tiYield();

    static uint32_t lastCheck = 0;
    if (millis() - lastCheck < 250) return;
    lastCheck = millis();

    if (WiFi.status() == WL_CONNECTED)
    {
      if (!s_announcedConnected)
      {
        s_announcedConnected = true;
        FLASH_TRACE_MARK("wifi-associated");   // RF calibration NVS write fires here
        Serial.printf("webfiles: WiFi up. IP=%s  RSSI=%ddBm\n",
                      WiFi.localIP().toString().c_str(),
                      (int)WiFi.RSSI());
        startServerOnce();
      }
    }
    else
    {
      s_announcedConnected = false;
      // No explicit reconnect — the ESP32 WiFi driver retries on its
      // own with the credentials WiFi.begin() stashed. Just keep ticking.
    }
  }

  bool setCredentials(const char* ssid, const char* pass)
  {
    if (!ssid || ssid[0] == '\0') return false;
    FLASH_TRACE_START("nvs-wifi-creds");
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, /*readonly=*/false))
    {
      FLASH_TRACE_END("nvs-wifi-creds");
      return false;
    }
    p.putString("ssid", ssid);
    p.putString("pass", pass ? pass : "");
    p.end();
    FLASH_TRACE_END("nvs-wifi-creds");
    connectWith(ssid, pass ? pass : "");
    return true;
  }

  void radioOff()
  {
    if (WiFi.getMode() == WIFI_OFF)
    {
      Serial.println("webfiles: radio already off");
      return;
    }
    if (s_serverRunning)
    {
      s_server.end();
      s_serverRunning = false;
    }
    WiFi.disconnect(true /*wifi_off*/, false /*keep config*/);
    WiFi.mode(WIFI_OFF);
    s_announcedConnected = false;
    Serial.println("webfiles: radio off");
  }

  void radioOn()
  {
    char ssid[40] = {0};
    char pass[72] = {0};
    if (!readCreds(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
      Serial.println("webfiles: radio on requested but no creds in NVS");
      return;
    }
    connectWith(ssid, pass);
  }

  void forget()
  {
    FLASH_TRACE_START("nvs-wifi-clear");
    Preferences p;
    if (p.begin(NVS_NAMESPACE, /*readonly=*/false))
    {
      p.clear();
      p.end();
    }
    FLASH_TRACE_END("nvs-wifi-clear");
    if (s_serverRunning)
    {
      s_server.end();
      s_serverRunning = false;
    }
    WiFi.disconnect(true, true);
    s_announcedConnected = false;
    Serial.println("webfiles: WiFi credentials cleared.");
  }

  void status(char* out, int outSize)
  {
    if (!out || outSize <= 0) return;
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED)
    {
      snprintf(out, outSize,
               "%s  IP=%s  RSSI=%ddBm  ONLINE",
               WiFi.SSID().c_str(),
               WiFi.localIP().toString().c_str(),
               (int)WiFi.RSSI());
    }
    else
    {
      // Distinguish "no creds at all" from "creds present, not connected"
      char ssid[40] = {0};
      char pass[72] = {0};
      bool haveCreds = readCreds(ssid, sizeof(ssid), pass, sizeof(pass));
      if (!haveCreds)
      {
        snprintf(out, outSize, "OFFLINE (no credentials set)");
      }
      else
      {
        snprintf(out, outSize, "%s  OFFLINE (status=%d)", ssid, (int)s);
      }
    }
  }

  void setBusy(bool busy)
  {
    s_busy = busy;
  }
}
