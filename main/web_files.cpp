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
#include <array>
#include "file_io.h"
#include "program_io.h"    // progio::copyFile / Target / CopyStatus
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
      if (fio::g_sdOk)
      {
        addDev("SDCARD", "SDCARD");
      }

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
        if (!fio::g_sdOk)
        {
          ok = false;
          error = "SD not present";
        }
        else
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

    // GET /api/file?dev=FLASH|SDCARD&path=NAME — stream a file to the
    // browser as an attachment download. DSK extract is handled by a
    // separate endpoint (/api/dskfile) below because the read path is
    // sector-based, not filesystem-based.
    s_server.on("/api/file", HTTP_GET, [](AsyncWebServerRequest* req) {
      String dev  = req->hasParam("dev")  ? req->getParam("dev")->value()  : "FLASH";
      String name = req->hasParam("path") ? req->getParam("path")->value() : "";

      // Path-traversal guard. We always serve from the filesystem root,
      // so reject anything with separators or '..'. Names that come from
      // /api/files are always bare filenames.
      if (name.length() == 0 ||
          name.indexOf('/')   >= 0 ||
          name.indexOf('\\')  >= 0 ||
          name.indexOf("..") >= 0)
      {
        req->send(400, "text/plain", "bad path");
        return;
      }

      fs::FS* fsRef = nullptr;
      if (dev.equalsIgnoreCase("FLASH"))
      {
        fsRef = &LittleFS;
      }
      else if (dev.equalsIgnoreCase("SDCARD") || dev.equalsIgnoreCase("SD"))
      {
        if (!fio::g_sdOk) { req->send(503, "text/plain", "SD not present"); return; }
        fsRef = &SD_MMC;
      }
      else
      {
        req->send(400, "text/plain", "unsupported device for /api/file (use /api/dskfile for DSK<n>)");
        return;
      }

      String fullPath = "/";
      fullPath += name;
      if (!fsRef->exists(fullPath))
      {
        req->send(404, "text/plain", "not found");
        return;
      }

      // beginResponse(fs, path, contentType="", download=true) sets
      // Content-Disposition: attachment so the browser saves rather
      // than tries to render. Empty content type lets the server pick
      // application/octet-stream — fine for .bas / .DSK / etc.
      AsyncWebServerResponse* resp =
          req->beginResponse(*fsRef, fullPath, String(), true);
      req->send(resp);
    });

    // DELETE /api/file?dev=FLASH|SDCARD&path=NAME — delete the file.
    // No DSK delete here: removing a file from a V9T9 catalog requires
    // rewriting the catalog + bitmap, which the DSK image library
    // handles via its own path (image->deleteFile()) — exposed by a
    // future /api/dskfile DELETE if needed.
    s_server.on("/api/file", HTTP_DELETE, [](AsyncWebServerRequest* req) {
      String dev  = req->hasParam("dev")  ? req->getParam("dev")->value()  : "FLASH";
      String name = req->hasParam("path") ? req->getParam("path")->value() : "";

      if (name.length() == 0 ||
          name.indexOf('/')   >= 0 ||
          name.indexOf('\\')  >= 0 ||
          name.indexOf("..") >= 0)
      {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"bad path\"}");
        return;
      }

      fs::FS* fsRef = nullptr;
      if (dev.equalsIgnoreCase("FLASH"))
      {
        fsRef = &LittleFS;
      }
      else if (dev.equalsIgnoreCase("SDCARD") || dev.equalsIgnoreCase("SD"))
      {
        if (!fio::g_sdOk)
        {
          req->send(503, "application/json", "{\"ok\":false,\"error\":\"SD not present\"}");
          return;
        }
        fsRef = &SD_MMC;
      }
      else
      {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"unsupported device\"}");
        return;
      }

      String fullPath = "/";
      fullPath += name;
      if (!fsRef->exists(fullPath))
      {
        req->send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
        return;
      }
      bool ok = fsRef->remove(fullPath);
      String body = String("{\"ok\":") + (ok ? "true" : "false");
      if (!ok) body += ",\"error\":\"remove failed\"";
      body += "}";
      req->send(ok ? 200 : 500, "application/json", body);
    });

    // POST /api/upload?dev=FLASH|SDCARD — multipart form upload. The
    // upload-chunk handler runs multiple times per file (index = 0 on
    // the first chunk, final = true on the last); we open the dest
    // file once at index 0 and stream writes through. State lives in
    // a module-static because the async server can only have one
    // upload in flight at a time anyway.
    static struct UploadCtx {
      File f;
      bool started;          // dest file successfully opened
      bool failed;           // any error seen since start
      String error;          // human-readable reason if failed
    } s_upload = { File(), false, false, "" };

    s_server.on("/api/upload", HTTP_POST,
      // Request-completion handler — runs after the upload chunk
      // stream finishes. Reports the outcome captured by the chunk
      // handler.
      [](AsyncWebServerRequest* req) {
        if (s_upload.failed)
        {
          String body = "{\"ok\":false,\"error\":\"";
          body += s_upload.error;
          body += "\"}";
          req->send(500, "application/json", body);
        }
        else if (s_upload.started)
        {
          req->send(200, "application/json", "{\"ok\":true}");
        }
        else
        {
          req->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"no file in request\"}");
        }
      },
      // Per-chunk upload handler.
      [](AsyncWebServerRequest* req, const String& filename,
         size_t index, uint8_t* data, size_t len, bool final) {
        if (index == 0)
        {
          // First chunk — reset state and open the destination.
          s_upload.failed  = false;
          s_upload.started = false;
          s_upload.error   = "";
          if (s_upload.f) { s_upload.f.close(); }

          // Path-traversal guard. The filename here is the original
          // form-field filename from the client; treat it as untrusted.
          if (filename.length() == 0 ||
              filename.indexOf('/')   >= 0 ||
              filename.indexOf('\\')  >= 0 ||
              filename.indexOf("..") >= 0)
          {
            s_upload.failed = true;
            s_upload.error  = "bad filename";
            return;
          }

          String dev = req->hasParam("dev") ? req->getParam("dev")->value() : "FLASH";
          fs::FS* fsRef = nullptr;
          if (dev.equalsIgnoreCase("FLASH"))
          {
            fsRef = &LittleFS;
          }
          else if (dev.equalsIgnoreCase("SDCARD") || dev.equalsIgnoreCase("SD"))
          {
            if (!fio::g_sdOk)
            {
              s_upload.failed = true;
              s_upload.error  = "SD not present";
              return;
            }
            fsRef = &SD_MMC;
          }
          else
          {
            s_upload.failed = true;
            s_upload.error  = "unsupported device";
            return;
          }

          String fullPath = "/";
          fullPath += filename;
          s_upload.f = fsRef->open(fullPath, "w");
          if (!s_upload.f)
          {
            s_upload.failed = true;
            s_upload.error  = "open failed";
            return;
          }
          s_upload.started = true;
        }

        if (s_upload.failed || !s_upload.started || !s_upload.f) return;

        if (len > 0)
        {
          size_t written = s_upload.f.write(data, len);
          if (written != len)
          {
            s_upload.failed = true;
            s_upload.error  = "write short";
            s_upload.f.close();
            return;
          }
        }

        if (final)
        {
          s_upload.f.flush();
          s_upload.f.close();
        }
      });

    // POST /api/mount?drive=N&spec=DEVICE.NAME — mount a .DSK image
    // into a virtual drive slot. `drive` is 1..MAX_DSK (35) or DSK<c>.
    // `spec` is the same form BASIC's MOUNT command takes:
    //   "FLASH.MYDISK"   — image file on internal LittleFS
    //   "SDCARD.MYDISK"  — image file on the SD card
    s_server.on("/api/mount", HTTP_POST, [](AsyncWebServerRequest* req) {
      String driveStr = req->hasParam("drive") ? req->getParam("drive")->value() : "";
      String spec     = req->hasParam("spec")  ? req->getParam("spec")->value()  : "";

      // Accept either "1", "DSK1", or "1" with separator dropped.
      int drive = 0;
      if (driveStr.length() > 0)
      {
        if (driveStr.equalsIgnoreCase("DSK") && driveStr.length() == 4)
        {
          drive = fio::driveFromChar(driveStr[3]);
        }
        else if (driveStr.length() >= 4 &&
                 (driveStr[0] == 'D' || driveStr[0] == 'd') &&
                 (driveStr[1] == 'S' || driveStr[1] == 's') &&
                 (driveStr[2] == 'K' || driveStr[2] == 'k'))
        {
          drive = fio::driveFromChar(driveStr[3]);
        }
        else
        {
          drive = driveStr.toInt();
        }
      }
      if (drive < 1 || drive > fio::MAX_DSK)
      {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"drive must be 1..35 or DSK1..DSKZ\"}");
        return;
      }
      if (spec.length() == 0)
      {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"missing spec\"}");
        return;
      }

      bool ok = fio::mountDskImage(drive, spec.c_str());
      // Persistence happens automatically inside fio::mountDskImage via
      // the mount-changed callback registered in setup() — no separate
      // saveMounts() call needed here.
      String body = String("{\"ok\":") + (ok ? "true" : "false");
      if (!ok)
      {
        body += ",\"error\":\"mount failed (file missing or not a DSK?)\"";
      }
      else
      {
        body += ",\"drive\":";
        body += drive;
      }
      body += "}";
      req->send(ok ? 200 : 500, "application/json", body);
    });

    // GET /api/dskfile?drive=N&path=NAME — extract a single file out
    // of a mounted V9T9 .DSK image and stream it as a download. Reads
    // sector-by-sector through the chunked-response callback, so no
    // big-buffer allocation is needed up front. Caller responsibility
    // is to know the file name from /api/files?dev=DSK<n>.
    s_server.on("/api/dskfile", HTTP_GET, [](AsyncWebServerRequest* req) {
      String driveStr = req->hasParam("drive") ? req->getParam("drive")->value() : "";
      String name     = req->hasParam("path")  ? req->getParam("path")->value()  : "";

      int drive = 0;
      if (driveStr.length() >= 4 &&
          (driveStr[0] == 'D' || driveStr[0] == 'd') &&
          (driveStr[1] == 'S' || driveStr[1] == 's') &&
          (driveStr[2] == 'K' || driveStr[2] == 'k'))
      {
        drive = fio::driveFromChar(driveStr[3]);
      }
      else
      {
        drive = driveStr.toInt();
      }
      if (drive < 1 || drive > fio::MAX_DSK)
      {
        req->send(400, "text/plain", "bad drive");
        return;
      }
      if (name.length() == 0)
      {
        req->send(400, "text/plain", "missing path");
        return;
      }

      dsk::DskImage* img = fio::dskImage(drive);
      if (!img)
      {
        req->send(503, "text/plain", "drive not mounted");
        return;
      }

      // Resolve the file's FDR. Names in the catalog are 10-char
      // space-padded; pad the request name to match.
      dsk::FileInfo info;
      if (!img->findFile(name.c_str(), info))
      {
        req->send(404, "text/plain", "file not in catalog");
        return;
      }

      // Compute precise byte length. The last sector may be partially
      // used; eofOffset gives the byte index of EOF in that sector.
      // (Matches readRawFile's accounting.)
      size_t totalBytes = (size_t)info.sectorCount * dsk::SECTOR_SIZE;
      if (info.eofOffset != 0 && info.sectorCount > 0)
      {
        totalBytes = (size_t)(info.sectorCount - 1) * dsk::SECTOR_SIZE + info.eofOffset;
      }

      // Streaming response. Lambda state tracks (a) which catalog
      // sector we're on, (b) how far through that sector's 256-byte
      // payload, and (c) the cached payload itself so a small maxLen
      // doesn't force us to re-read the same sector. `mutable` so the
      // captured cursors advance across calls. AwsResponseFiller:
      //   size_t(uint8_t* out, size_t maxLen, size_t alreadySent)
      // Return 0 to signal "done". The lambda lives inside the
      // AsyncWebServerResponse for the duration of the send.
      auto resp = req->beginChunkedResponse(
        "application/octet-stream",
        [drive, info, totalBytes,
         sectorIdx    = (uint16_t)0,
         sectorPos    = (size_t)0,            // 0..SECTOR_SIZE
         bytesEmitted = (size_t)0,
         sbuf         = std::array<uint8_t, dsk::SECTOR_SIZE>{},
         sbufValid    = false]
        (uint8_t* outBuf, size_t maxLen, size_t /*alreadySent*/) mutable -> size_t
        {
          if (bytesEmitted >= totalBytes) return 0;
          // Re-resolve the image each call — paranoid against an
          // unmount happening mid-stream.
          dsk::DskImage* im = fio::dskImage(drive);
          if (!im) return 0;

          // Pull in a fresh sector when we've drained the cached one.
          if (!sbufValid || sectorPos >= dsk::SECTOR_SIZE)
          {
            if (sectorIdx >= info.sectorCount) return 0;
            if (!im->readSector(info.sectors[sectorIdx], sbuf.data())) return 0;
            sectorIdx++;
            sectorPos = 0;
            sbufValid = true;
          }

          size_t remainingInSector = dsk::SECTOR_SIZE - sectorPos;
          size_t remainingInFile   = totalBytes - bytesEmitted;
          size_t want = remainingInSector;
          if (want > remainingInFile) want = remainingInFile;
          if (want > maxLen)          want = maxLen;
          memcpy(outBuf, sbuf.data() + sectorPos, want);
          sectorPos    += want;
          bytesEmitted += want;
          return want;
        });

      // Tell the browser to save rather than try to render.
      char disp[80];
      snprintf(disp, sizeof(disp),
               "attachment; filename=\"%s\"", name.c_str());
      resp->addHeader("Content-Disposition", disp);
      req->send(resp);
    });

    // POST /api/copy?src=DEV.NAME&dst=DEV.NAME — device-to-device copy.
    // Specs are the same form BASIC's COPY / SAVE / OLD accept. Calls
    // into the shared progio::copyFile() helper so the BASIC COPY
    // command and the web /api/copy go through the exact same code.
    s_server.on("/api/copy", HTTP_POST, [](AsyncWebServerRequest* req) {
      String src = req->hasParam("src") ? req->getParam("src")->value() : "";
      String dst = req->hasParam("dst") ? req->getParam("dst")->value() : "";
      if (src.length() == 0 || dst.length() == 0)
      {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"missing src or dst\"}");
        return;
      }
      progio::CopyStatus s = progio::copyFile(src.c_str(), dst.c_str());
      if (s == progio::COPY_OK)
      {
        req->send(200, "application/json", "{\"ok\":true}");
      }
      else
      {
        String body = "{\"ok\":false,\"error\":\"";
        body += progio::copyStatusMessage(s);
        body += "\"}";
        req->send(500, "application/json", body);
      }
    });

    // DELETE /api/mount?drive=N — unmount.
    s_server.on("/api/mount", HTTP_DELETE, [](AsyncWebServerRequest* req) {
      String driveStr = req->hasParam("drive") ? req->getParam("drive")->value() : "";
      int drive = 0;
      if (driveStr.length() >= 4 &&
          (driveStr[0] == 'D' || driveStr[0] == 'd') &&
          (driveStr[1] == 'S' || driveStr[1] == 's') &&
          (driveStr[2] == 'K' || driveStr[2] == 'k'))
      {
        drive = fio::driveFromChar(driveStr[3]);
      }
      else
      {
        drive = driveStr.toInt();
      }
      if (drive < 1 || drive > fio::MAX_DSK)
      {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"bad drive\"}");
        return;
      }
      fio::unmountDskImage(drive);
      // Persistence happens automatically inside fio::unmountDskImage.
      req->send(200, "application/json", "{\"ok\":true}");
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
        "body{font-family:monospace;background:#001;color:#cdf;margin:1em;max-width:64em;}"
        "h1{color:#fff;border-bottom:1px solid #468;padding-bottom:.3em;}"
        "h2{color:#fff;font-size:1em;border-bottom:1px solid #246;margin-top:1.5em;}"
        "input,select,button{font-family:monospace;background:#024;color:#cdf;"
        "border:1px solid #468;padding:.3em .6em;}"
        "input[type=text]{min-width:14em;}"
        "table{margin-top:.5em;border-collapse:collapse;width:100%;}"
        "th,td{padding:.2em .8em;text-align:left;border-bottom:1px solid #246;}"
        "th{color:#fff;}"
        "tr:hover{background:#012;}"
        "a{color:#9cf;text-decoration:none;}"
        "a:hover{text-decoration:underline;}"
        ".err{color:#f88;}"
        ".muted{color:#789;font-size:.9em;}"
        ".rt{text-align:right;}"
        ".del{cursor:pointer;background:transparent;border:none;padding:0 .4em;font-size:1.1em;opacity:.7;}"
        ".del:hover{opacity:1;}"
        "</style></head><body>"
        "<h1>TI Extended BASIC &mdash; File Manager</h1>"
        "<label>Device: <select id=dev></select></label> "
        "<button onclick=refreshAll()>Refresh</button>"
        " <button id=umbtn onclick=unmountCurrent() style='display:none'>Unmount this DSK</button>"
        "<div class=muted id=volinfo></div>"
        "<span class=muted id=status></span>"
        "<table id=ftbl><thead><tr>"
        "<th>File <span class=muted>(click name to download)</span></th>"
        "<th class=rt>Size</th>"
        "<th>Copy to</th>"
        "<th></th></tr>"
        "</thead><tbody></tbody></table>"
        "<h2>Upload to this device</h2>"
        "<input type=file id=upfile> <button onclick=upload()>Upload</button>"
        "<span class=muted id=upstatus></span>"
        "<h2>Mount a .DSK image as a virtual drive</h2>"
        "<label>Spec: <input type=text id=mountspec placeholder='FLASH.MYDISK or SDCARD.NAME'></label> "
        "<label>Drive: <select id=mountdrive></select></label> "
        "<button onclick=mount()>Mount</button>"
        "<span class=muted id=mountstatus></span>"
        "<script>"
        // human-readable byte sizes
        "function fmt(n){"
        "if(n<1024)return n+' B';"
        "if(n<1048576)return (n/1024).toFixed(1)+' KB';"
        "if(n<1073741824)return (n/1048576).toFixed(1)+' MB';"
        "return (n/1073741824).toFixed(2)+' GB';}"
        // True for DSK1..DSKZ device ids (case-insensitive).
        "function isDsk(d){return /^dsk[1-9a-z]$/i.test(d);}"
        // Drive number for DSK<c>: 1..9 then A=10..Z=35.
        "function driveOf(d){"
        "const c=d.slice(3).toUpperCase();"
        "if(c>='1'&&c<='9')return c.charCodeAt(0)-48;"
        "if(c>='A'&&c<='Z')return c.charCodeAt(0)-65+10;"
        "return 0;}"
        // Download URL for a file. DSK files go through the sector-streaming
        // /api/dskfile endpoint; flat FS files use /api/file.
        "function dlUrl(dev,name){"
        "if(isDsk(dev))return '/api/dskfile?drive='+driveOf(dev)+'&path='+encodeURIComponent(name);"
        "return '/api/file?dev='+encodeURIComponent(dev)+'&path='+encodeURIComponent(name);}"
        "async function delFile(name){"
        "const d=document.getElementById('dev').value;"
        "if(isDsk(d)){alert('Delete inside a DSK image is not supported via the web yet.');return;}"
        "if(!confirm('Delete '+name+' from '+d+'?'))return;"
        "const r=await fetch('/api/file?dev='+encodeURIComponent(d)+'&path='+encodeURIComponent(name),{method:'DELETE'});"
        "const j=await r.json();"
        "if(!j.ok){alert(j.error||'delete failed');return;}"
        "refresh();}"
        // Latest device list (populated by loadDevices); used by the
        // per-row Copy-to dropdown so a copy lands on FLASH/SDCARD/DSK.
        "let g_devs=[];"
        // Build source/dest specs for the COPY endpoint.
        // dev='FLASH'/'SDCARD'/'DSK1'..'DSKZ'; name is the listing's
        // literal filename (preserves any extension).
        "function makeSpec(dev,name){return dev+'.'+name;}"
        "async function copyTo(srcDev,srcName,dstDev){"
        "const s=document.getElementById('status');"
        "s.textContent='copying '+srcName+' to '+dstDev+'...';"
        "try{"
        "const r=await fetch('/api/copy?src='+encodeURIComponent(makeSpec(srcDev,srcName))+"
        "'&dst='+encodeURIComponent(makeSpec(dstDev,srcName)),{method:'POST'});"
        "const j=await r.json();"
        "if(!j.ok){s.innerHTML='<span class=err>copy: '+(j.error||'failed')+'</span>';return;}"
        "s.textContent='copied '+srcName+' to '+dstDev;"
        // If user copied to the currently-viewed device, refresh to
        // show the new file. Either way, no need to reload devices.
        "if(dstDev===document.getElementById('dev').value){refresh();}"
        "}catch(e){s.innerHTML='<span class=err>copy: '+e+'</span>';}}"
        "async function refresh(){"
        "const d=document.getElementById('dev').value;"
        "const s=document.getElementById('status');"
        "const vi=document.getElementById('volinfo');"
        "const tb=document.querySelector('#ftbl tbody');"
        // Show the unmount button only when viewing a DSK device.
        "document.getElementById('umbtn').style.display=isDsk(d)?'':'none';"
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
        "const a=document.createElement('a');"
        "a.href=dlUrl(d,f.name);a.textContent=f.name;a.download=f.name;"
        "const nameTd=document.createElement('td');nameTd.appendChild(a);"
        "const sizeTd=document.createElement('td');sizeTd.className='rt';sizeTd.textContent=fmt(f.size);"
        // Copy-to dropdown — lists every other device.
        "const copyTd=document.createElement('td');"
        "const cs=document.createElement('select');"
        "const optBlank=document.createElement('option');"
        "optBlank.value='';optBlank.textContent='\\u2014';cs.appendChild(optBlank);"
        "for(const dv of g_devs){"
        "if(dv.id===d)continue;"
        "const o=document.createElement('option');"
        "o.value=dv.id;o.textContent=dv.label;cs.appendChild(o);}"
        "cs.onchange=()=>{if(cs.value){copyTo(d,f.name,cs.value);cs.value='';}};"
        "copyTd.appendChild(cs);"
        // Delete button shown only for FLASH/SDCARD (DSK delete not yet wired).
        "const actTd=document.createElement('td');"
        "if(!isDsk(d)){"
        "const db=document.createElement('button');"
        // 🗑 (U+1F5D1 WASTEBASKET) — the trash can Apple famously has
        // strong feelings about. Falls back to the box glyph on
        // terminals that lack the emoji.
        "db.className='del';db.textContent='\\uD83D\\uDDD1';db.title='Delete';"
        "db.onclick=()=>delFile(f.name);"
        "actTd.appendChild(db);}"
        "tr.appendChild(nameTd);tr.appendChild(sizeTd);"
        "tr.appendChild(copyTd);tr.appendChild(actTd);"
        "tb.appendChild(tr);"
        "}}catch(e){s.innerHTML='<span class=err>'+e+'</span>';}}"
        // Populate the device dropdown from /api/devices on load.
        "async function loadDevices(){"
        "const sel=document.getElementById('dev');"
        "const prev=sel.value;"
        "try{"
        "const r=await fetch('/api/devices');"
        "const j=await r.json();"
        "g_devs=j.devices||[];"   // cache for per-row Copy-to dropdowns
        "sel.innerHTML='';"
        "for(const d of g_devs){"
        "const o=document.createElement('option');"
        "o.value=d.id;o.textContent=d.label;sel.appendChild(o);}"
        "if(prev){"
        "for(const o of sel.options){if(o.value===prev){sel.value=prev;break;}}}"
        "}catch(e){"
        "document.getElementById('status').innerHTML="
        "'<span class=err>devices: '+e+'</span>';}}"
        // Populate the mount-drive dropdown once with 1..35 (DSK1..DSKZ).
        "function initMountDrives(){"
        "const sel=document.getElementById('mountdrive');"
        "for(let i=1;i<=35;i++){"
        "const c=i<=9?String(i):String.fromCharCode(65+i-10);"
        "const o=document.createElement('option');o.value=i;o.textContent='DSK'+c;"
        "sel.appendChild(o);}}"
        // Mount a DSK image. Spec is the BASIC-style 'FLASH.NAME' or
        // 'SDCARD.NAME'. After success, refresh devices so the new DSK
        // appears in the dropdown.
        "async function mount(){"
        "const spec=document.getElementById('mountspec').value.trim();"
        "const drive=document.getElementById('mountdrive').value;"
        "const st=document.getElementById('mountstatus');"
        "if(!spec){st.innerHTML='<span class=err>need a spec</span>';return;}"
        "st.textContent='mounting...';"
        "try{"
        "const r=await fetch('/api/mount?drive='+drive+'&spec='+encodeURIComponent(spec),{method:'POST'});"
        "const j=await r.json();"
        "if(!j.ok){st.innerHTML='<span class=err>'+(j.error||'mount failed')+'</span>';return;}"
        "st.textContent='mounted on DSK'+(drive<=9?drive:String.fromCharCode(65+(drive-10)));"
        "await loadDevices();"
        "}catch(e){st.innerHTML='<span class=err>'+e+'</span>';}}"
        // Unmount the currently-selected DSK.
        "async function unmountCurrent(){"
        "const d=document.getElementById('dev').value;"
        "if(!isDsk(d))return;"
        "if(!confirm('Unmount '+d+'?'))return;"
        "try{"
        "const r=await fetch('/api/mount?drive='+driveOf(d),{method:'DELETE'});"
        "const j=await r.json();"
        "if(!j.ok){alert(j.error||'unmount failed');return;}"
        // Drop back to FLASH and refresh.
        "const sel=document.getElementById('dev');sel.value='FLASH';"
        "await refreshAll();"
        "}catch(e){alert(e);}}"
        // File upload via multipart POST. Browser handles the encoding.
        "async function upload(){"
        "const f=document.getElementById('upfile').files[0];"
        "const d=document.getElementById('dev').value;"
        "const st=document.getElementById('upstatus');"
        "if(!f){st.innerHTML='<span class=err>pick a file first</span>';return;}"
        "if(isDsk(d)){st.innerHTML='<span class=err>upload to a DSK image isn\\'t supported via the web yet</span>';return;}"
        "st.textContent='uploading '+f.name+' ('+fmt(f.size)+')...';"
        "const fd=new FormData();fd.append('file',f);"
        "try{"
        "const r=await fetch('/api/upload?dev='+encodeURIComponent(d),{method:'POST',body:fd});"
        "const j=await r.json();"
        "if(!j.ok){st.innerHTML='<span class=err>'+(j.error||'upload failed')+'</span>';return;}"
        "st.textContent='uploaded '+f.name;"
        "document.getElementById('upfile').value='';"
        "refresh();"
        "}catch(e){st.innerHTML='<span class=err>'+e+'</span>';}}"
        // refreshAll re-runs devices + files; Refresh button uses this
        // so a DSK mounted at the BASIC prompt becomes visible in the
        // dropdown without a full page reload.
        "async function refreshAll(){await loadDevices();await refresh();}"
        "document.getElementById('dev').onchange=refresh;"
        "initMountDrives();"
        "refreshAll();"
        "</script></body></html>";
      // Use the (uint8_t*, len) overload so the response holds a
      // pointer into the PROGMEM array rather than copying ~8 KB
      // into a heap String. Under WiFi + FS load the String copy
      // was failing silently and the page came back content-length:
      // 0 (white screen in the browser). PAGE is a static const
      // that outlives any request, so a pointer is safe.
      req->send(200, "text/html",
                reinterpret_cast<const uint8_t*>(PAGE),
                sizeof(PAGE) - 1);
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
