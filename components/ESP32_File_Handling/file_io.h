/*
 * TI BASIC Interpreter — File I/O
 *
 * Implements OPEN / CLOSE / PRINT # / INPUT # / LINPUT # / EOF() against
 * a small handle table routed to either LittleFS (FLASH1.) or SD card
 * (DSK1.). Scope: DISPLAY / SEQUENTIAL format only. INTERNAL / RELATIVE
 * / VARIABLE / FIXED are recognized in OPEN parsing but not supported;
 * OPEN returns an error if they're requested.
 *
 * Unit numbers 1..MAX_FILES (inclusive). Unit 0 is reserved.
 */

#ifndef FILE_IO_H
#define FILE_IO_H

#include <Arduino.h>
#include <LittleFS.h>
#include <FS.h>
#include "dsk_image.h"

// NOTE: this header is hardware-agnostic on purpose — it does NOT
// include <SD.h> or <SD_MMC.h>. Each project initializes whichever SD
// flavor matches its hardware (SD over SPI on the RGB/OTG boards,
// SD_MMC on the Box-3 SENSOR board) and hands the resulting fs::FS&
// to fio::setSDFs() at boot. file_io then routes SDCARD.* operations
// through that pointer.

namespace fio
{
  // User units 1..10 + two reserved for internal COPY source/dest.
  // (Real TI allowed 1..9 typically; we pick 10 to leave a little room
  // and still keep BSS small. ~700 bytes per Slot × 13 ≈ 9 KB.)
  static const int MAX_FILES = 12;
  static const int MAX_DSK   = 35;   // DSK1..DSK9, DSKA..DSKZ

  // Convert a TI drive character to a 1-based slot index.
  //   '1'..'9' -> 1..9
  //   'A'..'Z' / 'a'..'z' -> 10..35
  //   anything else -> 0
  inline int driveFromChar(char c)
  {
    if (c >= '1' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    return 0;
  }

  inline char driveToChar(int drive)
  {
    if (drive >= 1 && drive <= 9)  return (char)('0' + drive);
    if (drive >= 10 && drive <= 35) return (char)('A' + drive - 10);
    return '?';
  }

  enum Device : uint8_t { DEV_NONE = 0, DEV_FLASH = 1, DEV_SD = 2, DEV_DSK = 3 };
  enum Mode   : uint8_t { MODE_INPUT = 0, MODE_OUTPUT = 1, MODE_APPEND = 2, MODE_UPDATE = 3 };

  // Flags passed to openFile in addition to mode.
  static const int OF_INTERNAL = 0x01;   // TI radix-100 encoding (parsed; falls back to DISPLAY)
  static const int OF_FIXED    = 0x02;   // FIXED record length
  static const int OF_RELATIVE = 0x04;   // RELATIVE access (REC=K)

  struct Slot
  {
    bool    inUse = false;
    Device  device = DEV_NONE;
    Mode    mode   = MODE_INPUT;
    File    fh;
    char    path[48] = {0};
    bool    eof = false;   // sticky end-of-file for INPUT
    // When device == DEV_DSK, these drive the V9T9 reader/writer
    // instead of fh.
    int     dskDrive = 0;                 // 1..MAX_DSK
    dsk::DskImage::DisVarReader dskRdr{};
    dsk::DskImage::DisVarWriter dskWtr{};
    bool    dskWriting = false;
    // Open mode flags (FIXED / INTERNAL / RELATIVE) and FIXED-record
    // length / RELATIVE current record number. recLen == 0 means
    // VARIABLE.
    int     flags = 0;
    int     recLen = 0;
    long    curRecord = 0;
  };

  inline Slot g_slots[MAX_FILES + 1];   // index 1..MAX_FILES; [0] unused
  inline bool   g_sdOk = false;
  inline fs::FS* g_sdFs = nullptr;

  // Optional callback invoked when an in-flight SD operation fails —
  // typically because the card was yanked. The project installs this
  // to drop its cached SD-mount state so the next access re-probes
  // (lazy mount on access). file_io itself stays hardware-agnostic;
  // it just announces the failure and lets the project decide what to
  // do (e.g., call SD.end(), clear g_sdFs, etc.).
  typedef void (*SDInvalidateFn)();
  inline SDInvalidateFn g_sdInvalidate = nullptr;
  inline void setSDInvalidateCallback(SDInvalidateFn cb) { g_sdInvalidate = cb; }

  // Signal an SD failure. Called from internal failure paths; no-ops
  // when no callback is installed.
  inline void notifySDFailure()
  {
    if (g_sdInvalidate) g_sdInvalidate();
  }

  // Inject the SD filesystem after the project finishes its hardware-
  // specific bring-up. Pass nullptr to mark SD as unavailable.
  inline void setSDFs(fs::FS* fs)
  {
    g_sdFs = fs;
    g_sdOk = (fs != nullptr);
  }

  // Symmetric partner to setSDInvalidateCallback: the project supplies
  // a function that knows how to bring SD up from scratch (SD_MMC on
  // the Box-3, SD-over-SPI on the RGB-panel build, etc.). When SD has
  // been invalidated (or was never up) and a caller needs it, file_io
  // invokes this callback to attempt a fresh mount. The callback is
  // expected to call setSDFs() on success and return true.
  typedef bool (*SDInitFn)();
  inline SDInitFn g_sdInit = nullptr;
  inline void setSDInitCallback(SDInitFn cb) { g_sdInit = cb; }

  // Lazy-mount entry point. Callers that want to use SD should run this
  // first instead of checking g_sdOk directly: a stale invalidation
  // (e.g. card was yanked and then reinserted) will silently re-mount
  // on the next call. Returns true if SD is currently usable.
  inline bool ensureSdReady()
  {
    if (g_sdOk) return true;
    if (g_sdInit) return g_sdInit();
    return false;
  }

  // Mount table: DSK1..DSK3. Each slot points to a DskImage backed by
  // either LittleFS or the SD card. Unmounted slots have
  // isOpen() == false.
  struct Mount
  {
    dsk::DskImage img;
    // Display form of the source, e.g. "FLASH.SYSTEM.DSK" — used for
    // persistence and for the DIR header. Blank when unmounted.
    char     spec[48]      = {0};
    bool     mounted       = false;
    bool     fromFlash     = false;   // true=LittleFS, false=SD
    char     fsPath[48]    = {0};     // resolved path on the chosen fs
  };
  inline Mount g_mounts[MAX_DSK + 1];   // index 1..MAX_DSK; [0] unused

  // Parse a mount spec into (device, fsPath). Accepts:
  //   FLASH.NAME[.DSK]   -> fsPath = "/NAME.DSK" on LittleFS
  //   SDCARD.NAME[.DSK]  -> fsPath = "/NAME.DSK" on SD
  //   /ABS/PATH.DSK      -> fsPath = verbatim on SD (legacy)
  //   NAME[.DSK]         -> fsPath = "/NAME.DSK" on SD (legacy)
  // Returns true on success.
  inline bool resolveMountSpec(const char* spec, bool& fromFlash,
                               char* fsPath, int fsPathSize)
  {
    if (!spec) return false;
    const char* body = spec;
    if (strncasecmp(spec, "FLASH.", 6) == 0)
    {
      fromFlash = true; body = spec + 6;
    }
    else if (strncasecmp(spec, "FLASH1.", 7) == 0)
    {
      fromFlash = true; body = spec + 7;
    }
    else if (strncasecmp(spec, "SDCARD.", 7) == 0)
    {
      fromFlash = false; body = spec + 7;
    }
    else if (spec[0] == '/')
    {
      fromFlash = false;
      snprintf(fsPath, fsPathSize, "%s", spec);
      return true;
    }
    else
    {
      fromFlash = false;
      body = spec;
    }
    // Auto-append .DSK if no extension
    bool hasDot = strchr(body, '.') != NULL;
    snprintf(fsPath, fsPathSize, "/%s%s", body, hasDot ? "" : ".DSK");
    return true;
  }

  inline bool mountDskImage(int drive, const char* spec)
  {
    if (drive < 1 || drive > MAX_DSK) return false;
    Mount& m = g_mounts[drive];
    if (m.mounted) m.img.close();

    bool fromFlash;
    char fsPath[48];
    if (!resolveMountSpec(spec, fromFlash, fsPath, sizeof(fsPath)))
    {
      m.mounted = false; m.spec[0] = '\0';
      return false;
    }
    if (!fromFlash && !g_sdOk)
    {
      m.mounted = false; m.spec[0] = '\0';
      return false;
    }
    fs::FS& fs = fromFlash ? (fs::FS&)LittleFS : *g_sdFs;
    if (!m.img.open(fs, fsPath))
    {
      m.mounted = false; m.spec[0] = '\0';
      return false;
    }
    m.mounted = true;
    m.fromFlash = fromFlash;
    snprintf(m.spec, sizeof(m.spec), "%s", spec);
    snprintf(m.fsPath, sizeof(m.fsPath), "%s", fsPath);
    return true;
  }

  inline void unmountDskImage(int drive)
  {
    if (drive < 1 || drive > MAX_DSK) return;
    Mount& m = g_mounts[drive];
    if (m.mounted) m.img.close();
    m.mounted = false;
    m.spec[0] = '\0';
    m.fsPath[0] = '\0';
  }

  inline dsk::DskImage* dskImage(int drive)
  {
    if (drive < 1 || drive > MAX_DSK) return nullptr;
    Mount& m = g_mounts[drive];
    return m.mounted ? &m.img : nullptr;
  }

  inline const char* dskImagePath(int drive)
  {
    if (drive < 1 || drive > MAX_DSK) return "";
    return g_mounts[drive].spec;
  }

  // Parse "DEVICE.NAME" into (device, path, dskDrive). outPath is the
  // device-local name (without the slash prefix for DSK); dskDrive
  // is 1..3 when device == DEV_DSK, else 0.
  inline bool parseSpec(const char* spec, Device& dev, char* outPath,
                        int outSize, int& dskDrive)
  {
    if (!spec) return false;
    const char* dot = strchr(spec, '.');
    if (!dot) return false;
    int prefixLen = (int)(dot - spec);
    dskDrive = 0;
    // FLASH.NAME (canonical) and FLASH1.NAME (legacy alias) both map to
    // internal LittleFS.
    if ((prefixLen == 5 && strncasecmp(spec, "FLASH", 5) == 0) ||
        (prefixLen == 6 && strncasecmp(spec, "FLASH1", 6) == 0))
    {
      dev = DEV_FLASH;
      snprintf(outPath, outSize, "/%s", dot + 1);
      return true;
    }
    if (prefixLen == 6 && strncasecmp(spec, "SDCARD", 6) == 0)
    {
      dev = DEV_SD;
      snprintf(outPath, outSize, "/%s", dot + 1);
      return true;
    }
    if (prefixLen == 4 && strncasecmp(spec, "DSK", 3) == 0)
    {
      int d = driveFromChar(spec[3]);
      if (d > 0)
      {
        dev = DEV_DSK;
        dskDrive = d;
        snprintf(outPath, outSize, "%s", dot + 1);
        return true;
      }
    }
    return false;
  }

  inline bool unitValid(int unit) { return unit >= 1 && unit <= MAX_FILES; }

  // OPEN: returns 0 on success, non-zero error code otherwise.
  // err=1: bad unit, 2: already open, 3: bad spec, 4: device unavailable,
  // 5: unsupported mode, 6: file system error
  inline int openFile(int unit, const char* spec, Mode mode,
                      int flags = 0, int recLen = 0)
  {
    if (!unitValid(unit)) return 1;
    Slot& s = g_slots[unit];
    if (s.inUse) return 2;

    Device dev;
    char path[48];
    int drive = 0;
    if (!parseSpec(spec, dev, path, sizeof(path), drive)) return 3;

    if (dev == DEV_DSK)
    {
      dsk::DskImage* img = dskImage(drive);
      if (!img) return 4;
      if (mode == MODE_OUTPUT)
      {
        if (img->readOnly()) return 5;
        bool ok = (flags & OF_FIXED)
                    ? img->openFixedWriter(path, s.dskWtr)
                    : img->openDisVarWriter(path, s.dskWtr);
        if (!ok) return 6;
        s.inUse = true; s.device = DEV_DSK; s.mode = mode;
        s.eof = false; s.dskDrive = drive; s.dskWriting = true;
        s.flags = flags; s.recLen = recLen; s.curRecord = 0;
        snprintf(s.path, sizeof(s.path), "%s", path);
        return 0;
      }
      if (mode == MODE_INPUT)
      {
        dsk::FileInfo info;
        if (!img->findFile(path, info)) return 6;
        if (info.flags & 0x01) return 5;    // PROGRAM
        if (info.flags & 0x02) return 5;    // INTERNAL not supported on read
        s.inUse = true; s.device = DEV_DSK; s.mode = mode;
        s.eof = false; s.dskDrive = drive; s.dskWriting = false;
        s.flags = flags;
        // FIXED: prefer caller-supplied recLen, but fall back to FDR.
        s.recLen = recLen ? recLen : info.recLen;
        s.curRecord = 0;
        snprintf(s.path, sizeof(s.path), "%s", path);
        if (!img->openDisVarReader(info, s.dskRdr)) s.eof = true;
        return 0;
      }
      return 5;   // APPEND / UPDATE on DSK not implemented in Phase 2
    }

    const char* openMode = "r";
    if (mode == MODE_OUTPUT)      openMode = "w";
    else if (mode == MODE_APPEND) openMode = "a";
    else if (mode == MODE_UPDATE) openMode = "r+";

    // FIXED-record output may seek backwards (RELATIVE) or past EOF, so
    // we need a handle that supports both reading-back and seeking. "w+"
    // is identical to "w" except it also opens for read, which is what
    // FS layers need to allow arbitrary seeks.
    if (mode == MODE_OUTPUT && (flags & OF_FIXED)) openMode = "w+";

    File fh;
    if (dev == DEV_FLASH)
    {
      fh = LittleFS.open(path, openMode);
      if (!fh) return 6;
    }
    else
    {
      // No SD fs registered = device truly absent (e.g. card not in
      // slot at boot). Distinct from "card mounted but file missing"
      // so the caller can map to TI I/O ERROR 05 (device) vs 06 (file).
      if (!g_sdFs) return 4;
      fh = g_sdFs->open(path, openMode);
      if (!fh)
      {
        // SD was mounted at this layer but the FS rejected the open.
        // Most common cause on this hardware is "card was yanked
        // since the last successful op" — invalidate the cached mount
        // so the next access re-probes. The current call still
        // returns 6 (file error) since we genuinely can't tell
        // file-not-found from gone-card without a separate probe.
        notifySDFailure();
        return 6;
      }
    }

    s.inUse = true;
    s.device = dev;
    s.mode = mode;
    s.fh = fh;
    s.eof = false;
    s.flags = flags;
    s.recLen = recLen;
    s.curRecord = 0;
    snprintf(s.path, sizeof(s.path), "%s", path);
    return 0;
  }

  // Seek a FIXED-record file to record number `rec`. For DSK images the
  // actual seek happens at I/O time (the writer/reader honors curRecord
  // when OF_RELATIVE is set); for FLASH/SD we do a byte seek now.
  // Returns false on bad unit or seek failure.
  inline bool seekRecord(int unit, long rec)
  {
    if (!unitValid(unit) || !g_slots[unit].inUse) return false;
    Slot& s = g_slots[unit];
    if (s.recLen <= 0) return false;
    s.curRecord = rec;
    // An explicit REC=K is itself a RELATIVE-style positioning request,
    // even if the file was OPENed without RELATIVE — tag it so the DSK
    // path honors targetRec on the next I/O.
    s.flags |= OF_RELATIVE;
    if (s.device == DEV_DSK) return true;
    uint32_t offset = (uint32_t)rec * (uint32_t)s.recLen;
    // If we're seeking past EOF on a writable handle, pad the file out
    // to the target with spaces so the seek lands on real bytes.
    // LittleFS won't extend a file via plain seek; subsequent writes
    // would silently land at position 0 otherwise.
    if (s.mode == MODE_OUTPUT || s.mode == MODE_APPEND ||
        s.mode == MODE_UPDATE)
    {
      uint32_t curSize = (uint32_t)s.fh.size();
      if (offset > curSize)
      {
        s.fh.seek(curSize);
        uint8_t pad[16];
        memset(pad, ' ', sizeof(pad));
        uint32_t need = offset - curSize;
        while (need > 0)
        {
          uint32_t chunk = need > sizeof(pad) ? sizeof(pad) : need;
          s.fh.write(pad, chunk);
          need -= chunk;
        }
        s.fh.flush();
      }
    }
    return s.fh.seek(offset);
  }

  // RESTORE #n — rewind an open file to its first record without
  // closing it. Resets eof, curRecord, and the DSK reader's logical
  // position; for FLASH/SD-backed handles it byte-seeks to 0.
  // Returns false on bad unit / closed unit / non-input mode.
  inline bool rewindFile(int unit)
  {
    if (!unitValid(unit) || !g_slots[unit].inUse) return false;
    Slot& s = g_slots[unit];
    s.eof = false;
    s.curRecord = 0;
    if (s.device == DEV_DSK)
    {
      // Re-init the DIS/VAR reader from the original FileInfo. We
      // didn't save the FileInfo on open, but the reader struct holds
      // a copy in dskRdr.info — so we just reset its cursor fields.
      s.dskRdr.curLogicalSector = 0;
      s.dskRdr.posInSector = 0;
      s.dskRdr.bufLoaded = false;
      s.dskRdr.recordsRead = 0;
      s.dskRdr.eof = (s.dskRdr.info.sectorCount == 0 ||
                     s.dskRdr.info.numRecords == 0);
      return true;
    }
    return s.fh.seek(0);
  }

  inline int closeFile(int unit)
  {
    if (!unitValid(unit)) return 1;
    Slot& s = g_slots[unit];
    if (!s.inUse) return 0;   // closing an unopen unit is a no-op
    if (s.device == DEV_DSK)
    {
      if (s.dskWriting)
      {
        dsk::DskImage* img = dskImage(s.dskDrive);
        if (img)
        {
          if (s.flags & OF_FIXED) img->closeFixedWriter(s.dskWtr, s.recLen);
          else                    img->closeDisVarWriter(s.dskWtr);
        }
      }
    }
    else
    {
      s.fh.close();
    }
    s.inUse = false;
    s.device = DEV_NONE;
    s.eof = false;
    s.dskDrive = 0;
    s.dskWriting = false;
    s.flags = 0;
    s.recLen = 0;
    s.curRecord = 0;
    s.path[0] = '\0';
    return 0;
  }

  inline int printLineTo(int unit, const char* text)
  {
    if (!unitValid(unit) || !g_slots[unit].inUse) return 1;
    Slot& s = g_slots[unit];
    if (s.mode == MODE_INPUT) return 5;

    if (s.device == DEV_DSK)
    {
      dsk::DskImage* img = dskImage(s.dskDrive);
      if (!img) return 4;
      if (s.flags & OF_FIXED)
      {
        return img->writeFixedRecord(s.dskWtr, text, s.recLen,
                                     (s.flags & OF_RELATIVE) ? &s.curRecord
                                                             : nullptr)
                  ? 0 : 6;
      }
      return img->writeDisVarRecord(s.dskWtr, text) ? 0 : 6;
    }

    if (s.flags & OF_FIXED)
    {
      // FIXED: write exactly recLen bytes, space-padded or truncated.
      // RELATIVE: caller has already seekRecord'd; we just write at the
      // current position and bump curRecord.
      int n = (int)strlen(text);
      if (n > s.recLen) n = s.recLen;
      size_t got = s.fh.write((const uint8_t*)text, n);
      if ((int)got != n)
      {
        if (s.device == DEV_SD) notifySDFailure();
        return 6;
      }
      for (int i = n; i < s.recLen; i++) s.fh.write((uint8_t)' ');
      s.fh.flush();
      s.curRecord++;
      return 0;
    }

    int len = (int)strlen(text);
    size_t got = s.fh.write((const uint8_t*)text, len);
    if ((int)got != len)
    {
      if (s.device == DEV_SD) notifySDFailure();
      return 6;
    }
    s.fh.write((uint8_t)'\n');
    s.fh.flush();
    return 0;
  }

  // Read a line (LF-terminated) into buf. Returns 0 on success, non-zero
  // on EOF or error. Strips trailing \r\n.
  inline int readLineFrom(int unit, char* buf, int bufSize)
  {
    if (!unitValid(unit) || !g_slots[unit].inUse) return 1;
    Slot& s = g_slots[unit];
    if (s.mode != MODE_INPUT && s.mode != MODE_UPDATE) return 5;

    if (s.device == DEV_DSK)
    {
      dsk::DskImage* img = dskImage(s.dskDrive);
      if (!img) { s.eof = true; buf[0] = '\0'; return 2; }
      if (s.flags & OF_FIXED)
      {
        int len = img->readFixedRecord(s.dskRdr, buf, bufSize, s.recLen,
                                       (s.flags & OF_RELATIVE) ? &s.curRecord
                                                               : nullptr);
        if (len < 0) { s.eof = true; buf[0] = '\0'; return 2; }
        if (s.dskRdr.eof) s.eof = true;
        return 0;
      }
      int len = img->readDisVarRecord(s.dskRdr, buf, bufSize);
      if (len < 0) { s.eof = true; buf[0] = '\0'; return 2; }
      if (s.dskRdr.eof) s.eof = true;
      return 0;
    }

    if (s.flags & OF_FIXED)
    {
      // FIXED: read exactly recLen bytes, strip trailing spaces.
      if (!s.fh.available())
      {
        s.eof = true; buf[0] = '\0'; return 2;
      }
      int want = s.recLen;
      if (want > bufSize - 1) want = bufSize - 1;
      int n = s.fh.read((uint8_t*)buf, want);
      if (n <= 0) { s.eof = true; buf[0] = '\0'; return 2; }
      // Pop trailing spaces (FIXED-mode pad) so caller sees the logical text.
      while (n > 0 && buf[n - 1] == ' ') n--;
      buf[n] = '\0';
      s.curRecord++;
      if (!s.fh.available()) s.eof = true;
      return 0;
    }

    if (!s.fh.available())
    {
      s.eof = true;
      buf[0] = '\0';
      return 2;
    }
    int n = 0;
    while (s.fh.available() && n < bufSize - 1)
    {
      int c = s.fh.read();
      if (c < 0) break;
      if (c == '\n') break;
      if (c == '\r') continue;
      buf[n++] = (char)c;
    }
    buf[n] = '\0';
    if (!s.fh.available()) s.eof = true;
    return 0;
  }

  inline bool isEof(int unit)
  {
    if (!unitValid(unit)) return true;
    Slot& s = g_slots[unit];
    if (!s.inUse) return true;
    if (s.device == DEV_DSK) return s.eof || s.dskRdr.eof;
    return s.eof || !s.fh.available();
  }

  inline void closeAll()
  {
    for (int i = 1; i <= MAX_FILES; i++) closeFile(i);
  }

  // TI Extended BASIC I/O error codes — the two-digit number that appears
  // after "I/O ERROR" in run-mode and that CALL ERR returns. Reference:
  // TI Extended BASIC manual, Appendix A.
  enum TIIoError : int
  {
    TI_IO_WRITE_PROTECT = 0,
    TI_IO_BAD_ATTR      = 1,
    TI_IO_ILLEGAL_OP    = 2,
    TI_IO_OUT_OF_SPACE  = 3,
    TI_IO_PAST_EOF      = 4,
    TI_IO_DEVICE_ERROR  = 5,   // device missing / hardware failure
    TI_IO_FILE_ERROR    = 6,   // file not found / bad type / FS corruption
    TI_IO_ALREADY_OPEN  = 7,
  };

  // Translate an internal file_io return code (1..6, see openFile/printLineTo
  // /readLineFrom doc comments) to the TI two-digit I/O error code. `unit`
  // is the file slot the error came from — needed to tell device-error from
  // file-error (an FS error on a DSK or SD slot is a device problem, on
  // FLASH it's a file problem). Pass unit=0 for unit-agnostic errors.
  inline int toTIError(int fioCode, int unit = 0)
  {
    switch (fioCode)
    {
      case 0: return -1;   // success — caller should not raise an error
      case 1: return TI_IO_ILLEGAL_OP;     // bad unit number
      case 2: return TI_IO_ALREADY_OPEN;
      case 3: return TI_IO_BAD_ATTR;       // bad spec parse
      case 4: return TI_IO_DEVICE_ERROR;   // device unavailable
      case 5: return TI_IO_BAD_ATTR;       // unsupported mode/format
      case 6:
        // FS-layer failure: device error if the slot is on removable
        // media, file error if on internal flash.
        if (unitValid(unit) && g_slots[unit].inUse)
        {
          Device d = g_slots[unit].device;
          if (d == DEV_SD || d == DEV_DSK) return TI_IO_DEVICE_ERROR;
        }
        return TI_IO_FILE_ERROR;
      default: return TI_IO_FILE_ERROR;
    }
  }
}

#endif // FILE_IO_H
