/*
 * ESP32_File_Handling — program SAVE / OLD device routing + V9T9 binary
 *
 * Higher-level glue on top of file_io.h that resolves a TI-style SAVE /
 * OLD / MERGE / DELETE filespec ("FLASH.NAME", "SDCARD.NAME", "DSK1.NAME",
 * bare "NAME") into a (filesystem, path) pair or a (DskImage, tiName)
 * pair. Also provides the V9T9 PROGRAM-format binary serializer used
 * when the target is a mounted .dsk image (TI cartridges and TI-99/4A
 * disk programs all use this layout).
 *
 * The interpreter side (cmdSave / cmdOld in each project's .ino) used
 * to inline this routing. Promoting it to a library here means new
 * sibling projects pick it up by bumping the submodule, and bug fixes
 * land once instead of N times.
 *
 * Decoupling from the language layer:
 *   The V9T9 functions don't know about ExecManager or ProgramLine.
 *   They take function-pointer callbacks (progio::LineSource for save,
 *   progio::LineSink for load) so each project supplies its own line
 *   accessors. The library only cares about (line number, token bytes,
 *   length).
 *
 * Hardware-agnostic SD: same as file_io.h, this header doesn't include
 * <SD.h> or <SD_MMC.h>. Callers pass in the SD fs::FS& they want
 * (typically just SD or SD_MMC at the call site).
 */

#ifndef PROGRAM_IO_H
#define PROGRAM_IO_H

#include <Arduino.h>
#include <FS.h>
#include "dsk_image.h"
#include "file_io.h"

namespace progio
{
  // A filespec parses into one of these.
  enum Kind : uint8_t
  {
    KIND_FLASH  = 0,
    KIND_SDCARD = 1,
    KIND_DSK    = 2
  };

  struct Target
  {
    Kind kind;
    // For KIND_FLASH / KIND_SDCARD: the path under root, no leading slash
    // (e.g., "CANON" or "FOO.BAS"). The bare-name → ".bas" fallback is
    // applied by resolveExistingPath() on the load path, and by the
    // SAVE call site (which always appends .bas itself).
    char  name[40];
    // For KIND_DSK: 1..35 (driveFromChar()'s output range).
    int   drive;
    // For KIND_DSK: the 10-char TI filename (no path).
    char  tiName[16];
  };

  // Is `name` a reserved device-prefix token? FLASH, SDCARD, and the
  // 35 DSK<n> drive specs (DSK1..DSK9, DSKA..DSKZ) can never be the
  // file portion of a filespec — bare or prefixed — because the user
  // almost certainly meant the device, not a file literally named
  // after it. Case-insensitive.
  inline bool isReservedName(const char* name)
  {
    if (!name) return false;
    if (strcasecmp(name, "FLASH")  == 0) return true;
    if (strcasecmp(name, "SDCARD") == 0) return true;
    // DSK + single digit (1..9) or single letter (A..Z) is reserved
    // whether or not it's the start of a device spec.
    if ((name[0] == 'D' || name[0] == 'd') &&
        (name[1] == 'S' || name[1] == 's') &&
        (name[2] == 'K' || name[2] == 'k') &&
        name[3] != '\0' && name[4] == '\0')
    {
      return fio::driveFromChar(name[3]) > 0;
    }
    return false;
  }

  // Parse "FLASH.NAME" / "SDCARD.NAME" / "DSKn.NAME" / bare "NAME".
  // Bare names default to FLASH (back-compat with pre-routing programs).
  // Empty input, missing file portion after a prefix, or file portion
  // equal to a reserved token all return false (caller treats as
  // * BAD FILE NAME).
  inline bool parseTarget(const char* spec, Target& out)
  {
    if (!spec || spec[0] == '\0') return false;

    // DSKn.NAME — n is '1'..'9' or 'A'..'Z' (1..35).
    if (strncasecmp(spec, "DSK", 3) == 0)
    {
      int drive = fio::driveFromChar(spec[3]);
      if (drive > 0 && spec[4] == '.')
      {
        const char* tiName = spec + 5;
        if (tiName[0] == '\0' || isReservedName(tiName)) return false;
        out.kind  = KIND_DSK;
        out.drive = drive;
        strncpy(out.tiName, tiName, sizeof(out.tiName) - 1);
        out.tiName[sizeof(out.tiName) - 1] = '\0';
        return true;
      }
      // "DSK<x>." with no x, or "DSK<x>" without a dot, falls through
      // to the flat-fs path. Probably a user typo; let it fail later.
    }

    if (strncasecmp(spec, "FLASH.", 6) == 0)
    {
      const char* name = spec + 6;
      if (name[0] == '\0' || isReservedName(name)) return false;
      out.kind = KIND_FLASH;
      strncpy(out.name, name, sizeof(out.name) - 1);
      out.name[sizeof(out.name) - 1] = '\0';
      return true;
    }
    if (strncasecmp(spec, "SDCARD.", 7) == 0)
    {
      const char* name = spec + 7;
      if (name[0] == '\0' || isReservedName(name)) return false;
      out.kind = KIND_SDCARD;
      strncpy(out.name, name, sizeof(out.name) - 1);
      out.name[sizeof(out.name) - 1] = '\0';
      return true;
    }

    // Bare name → FLASH (back-compat). Reject bare reserved tokens —
    // SAVE "SDCARD" or SAVE "DSK1" almost certainly mean the user
    // typed a device name where they meant a filename.
    if (isReservedName(spec)) return false;
    out.kind = KIND_FLASH;
    strncpy(out.name, spec, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = '\0';
    return true;
  }

  // Smart path resolution for OLD / MERGE / DELETE on flat filesystems
  // (FLASH and SDCARD — not DSK images, which have their own naming).
  // Build a path under root for `name`, then prefer the exact name; if
  // that doesn't exist, fall back to <name>.bas. Handles three cases:
  //   1. File saved via SAVE → has ".bas" appended by cmdSave. Looking
  //      up "FOO" finds "FOO.bas".
  //   2. File dropped onto SD from a PC with an extension. "FOO.bas"
  //      looked up as-is, "CONFIG" looked up bare. Both work.
  //   3. Externally-created no-extension file (e.g., "README") —
  //      "FLASH.README" finds it because bare lookup wins.
  // Returns true if a file was found at one of the candidates.
  inline bool resolveExistingPath(fs::FS& fs, const char* name,
                                  char* outPath, int outSize)
  {
    if (!name || name[0] == '\0') return false;
    // Try the literal name first.
    snprintf(outPath, outSize, "/%s", name);
    if (fs.exists(outPath)) return true;
    // Fall back to <name>.bas — but only if the user didn't already
    // supply an extension. If they typed "FOO.txt" and that doesn't
    // exist, we should not silently try "FOO.txt.bas".
    if (strchr(name, '.') != NULL) return false;
    snprintf(outPath, outSize, "/%s.bas", name);
    return fs.exists(outPath);
  }

  // Callbacks for the V9T9 binary serializer. The library has no
  // built-in notion of "a program"; callers supply these to bridge
  // their language-layer types (ExecManager, ProgramLine, etc.).
  //
  // LineSource: called by saveProgramBytes to enumerate lines.
  //   count: total number of lines in the program (must be > 0).
  //   get:   for index 0..count-1, fill in line number, token bytes
  //          pointer, and length. The token bytes pointer must remain
  //          valid for the duration of saveProgramBytes.
  // LineSink: called by loadProgramBytes to populate a fresh program.
  //   clear: wipe the current program first.
  //   store: store one line. toks is a temporary buffer; the
  //          implementation should copy if needed.
  struct LineSource
  {
    int  (*count)();
    void (*get)(int idx, int* outLineNum,
                const uint8_t** outToks, int* outLen);
  };
  struct LineSink
  {
    void (*clear)();
    void (*store)(int lineNum, const uint8_t* toks, int len);
  };

  // Serialize the program described by `src` into V9T9 PROGRAM format
  // (the layout used by TI-99/4A cartridges and saved disk files).
  // Returns the number of bytes written, or -1 on buffer overflow / OOM.
  //
  // Format: 8-byte header (cksum, LNT top, LNT bot, prog top — all
  // 16-bit big-endian), then the line-number table (4 bytes per line,
  // descending by line number), then program text. Each line is stored
  // as [length byte][tokens...][0x00 terminator]; the LNT pointer
  // points one byte past the length (the first token).
  //
  // Caller is responsible for stripping any TOK_EOL terminator at the
  // end of their token stream — the V9T9 format uses 0x00 not TOK_EOL,
  // and the length byte counts the 0x00 terminator. The library does
  // the right thing if `outLen` doesn't include a terminator (most
  // callers' "raw tokens" representation).
  //
  // Implementation: see program_io.cpp.
  int  saveProgramBytes(const LineSource& src,
                        uint8_t* buf, int bufSize);

  // Parse V9T9 PROGRAM-format bytes into the program described by
  // `dst`. Calls dst.clear() first, then dst.store() once per line.
  // Returns true if at least one line was decoded successfully.
  //
  // maxTokens caps the per-line token count the caller is willing to
  // accept (typically the project's MAX_LINE_TOKENS). Lines longer
  // than that are silently skipped — the library doesn't know the
  // caller's storage limits.
  //
  // Robustness: handles both "length includes 0x00" and "length excludes
  // 0x00" variants (different writers use different conventions).
  bool loadProgramBytes(const uint8_t* buf, int size,
                        const LineSink& dst, int maxTokens);
}

#endif // PROGRAM_IO_H
