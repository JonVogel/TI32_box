/*
 * TI-99/4A V9T9 DSK Image Reader  (Phase 1: read-only DIS/VAR)
 *
 * Spec reference:
 *   https://www.unige.ch/medecine/nouspikel/ti99/disks.htm
 *   https://www.ninerpedia.org/wiki/Floppy_disk_file_system
 *
 * SSSD layout (what we support for now; DSSD/DSDD are the same shape
 * with more sectors):
 *   Sector 0 : Volume Information Block (VIB)
 *   Sector 1 : File directory — 128 big-endian word pointers to FDRs
 *              (one per file, 0x0000 = end)
 *   Sector 2 : (not fixed — first available sector for files/FDRs)
 *   ...
 *
 * FDR (File Descriptor Record), 1 sector per file:
 *   0x00-0x09 : filename (10 chars, space-padded)
 *   0x0A-0x0B : reserved (typically 0)
 *   0x0C      : status flags
 *                 0x01 = PROGRAM (raw memory image, no records)
 *                 0x02 = INTERNAL  (0 = DISPLAY)
 *                 0x08 = PROTECTED
 *                 0x20 = MODIFIED
 *                 0x40 = VARIABLE  (0 = FIXED)
 *                 0x80 = "emulated" / not-a-real-disk
 *   0x0D      : records-per-sector (FIXED) or 0
 *   0x0E-0x0F : total sectors allocated to file (big-endian word)
 *   0x10      : EOF offset (byte offset in last sector; 0 = full last)
 *   0x11      : logical record length (FIXED) or 0
 *   0x12-0x13 : number of records (little-endian! a classic TI quirk)
 *   0x14-0x1B : reserved
 *   0x1C-0xFF : cluster chain — 3 bytes per cluster:
 *                   byte0 : start-sector low
 *                   byte1 : (offset high nibble) | (start-sector high nibble)
 *                   byte2 : end-offset low
 *               where offset = logical sector number within the file
 *
 * Record format for DISPLAY/VARIABLE:
 *   Each record = [length byte] [length bytes of data]
 *   Records never span sector boundaries. Remaining bytes in a sector
 *   are padded with 0x00. End-of-file marked by 0xFF length byte
 *   (since valid lengths are 0..254).
 */

#ifndef DSK_IMAGE_H
#define DSK_IMAGE_H

#include <Arduino.h>
#include <FS.h>

namespace dsk
{
  static const int SECTOR_SIZE = 256;

  struct Vib
  {
    char    name[11];        // volume name, null-terminated copy
    uint16_t totalSectors;
    uint8_t  sectorsPerTrack;
    uint8_t  tracksPerSide;
    uint8_t  sides;
    uint8_t  density;        // 1=SD, 2=DD
  };

  // A single file as tracked inside the image. Only populated for files
  // the caller has actually opened / listed.
  struct FileInfo
  {
    char     name[11];       // 10-char TI name, null-terminated
    uint16_t fdrSector;      // where the FDR lives
    uint16_t totalSectors;   // sectors allocated
    uint8_t  flags;
    uint16_t numRecords;
    uint8_t  eofOffset;
    uint8_t  recLen;
    // Cluster chain expanded into a list of physical sectors (one per
    // logical sector in the file). Up to 128 sectors = 32 KB, which is
    // plenty for DIS/VAR files on SSSD.
    uint16_t sectorCount;
    uint16_t sectors[128];
  };

  class DskImage
  {
  public:
    DskImage() {}
    ~DskImage() { close(); }

    bool isOpen() const { return m_fh; }
    bool readOnly() const { return m_ro; }
    const Vib& vib() const { return m_vib; }

    // Open an existing .dsk file. Tries read-write first, falls back to
    // read-only. Caller supplies the fs (LittleFS or SD).
    // openReason codes: 0=ok, 1=open failed, 2=read sector 0 failed,
    // 3=bad VIB marker
    int openReason = 0;

    bool open(fs::FS& fs, const char* path)
    {
      close();
      openReason = 0;
      m_fs = &fs;
      m_fh = fs.open(path, "r+");
      if (!m_fh) { m_fh = fs.open(path, "r"); m_ro = true; }
      else       { m_ro = false; }
      if (!m_fh) { openReason = 1; return false; }
      m_path[0] = '\0';
      snprintf(m_path, sizeof(m_path), "%s", path);

      uint8_t s0[SECTOR_SIZE];
      if (!readSector(0, s0))
      {
        openReason = 2;
        close();
        return false;
      }
      // Parse VIB
      memcpy(m_vib.name, s0, 10); m_vib.name[10] = '\0';
      m_vib.totalSectors    = (uint16_t(s0[10]) << 8) | s0[11];
      m_vib.sectorsPerTrack = s0[12];
      m_vib.tracksPerSide   = s0[17];
      m_vib.sides           = s0[18];
      m_vib.density         = s0[19];

      // Sanity check: marker "DSK" at bytes 13..15 identifies a valid VIB
      if (s0[13] != 'D' || s0[14] != 'S' || s0[15] != 'K')
      {
        openReason = 3;
        // Dump the first 16 bytes to serial for debugging
        Serial.print("VIB bytes 0-15:");
        for (int i = 0; i < 16; i++) Serial.printf(" %02X", s0[i]);
        Serial.println();
        close();
        return false;
      }
      return true;
    }

    void close()
    {
      if (m_fh)
      {
        m_fh.close();
      }
      m_fh = File();
      m_ro = true;
    }

    // List all files on the disk. Returns count; fills names (each
    // 11-byte buffer). maxFiles bounds the array.
    int listFiles(char names[][11], int maxFiles)
    {
      uint8_t dir[SECTOR_SIZE];
      if (!readSector(1, dir)) return 0;

      int count = 0;
      for (int i = 0; i < 128 && count < maxFiles; i++)
      {
        uint16_t fdrSec = (uint16_t(dir[i * 2]) << 8) | dir[i * 2 + 1];
        if (fdrSec == 0) break;

        uint8_t fdr[SECTOR_SIZE];
        if (!readSector(fdrSec, fdr)) continue;
        memcpy(names[count], fdr, 10);
        names[count][10] = '\0';
        // Trim trailing spaces
        for (int k = 9; k >= 0 && names[count][k] == ' '; k--)
        {
          names[count][k] = '\0';
        }
        count++;
      }
      return count;
    }

    // Compact catalog entry — what TI Disk Manager shows per file.
    struct CatEntry
    {
      char     name[11];
      uint8_t  flags;
      uint16_t totalSectors;
      uint8_t  recLen;          // 0 for PROGRAM
      uint16_t numRecords;
    };

    int listCatalog(CatEntry* entries, int maxEntries)
    {
      uint8_t dir[SECTOR_SIZE];
      if (!readSector(1, dir)) return 0;

      int count = 0;
      for (int i = 0; i < 128 && count < maxEntries; i++)
      {
        uint16_t fdrSec = (uint16_t(dir[i * 2]) << 8) | dir[i * 2 + 1];
        if (fdrSec == 0) break;

        uint8_t fdr[SECTOR_SIZE];
        if (!readSector(fdrSec, fdr)) continue;

        CatEntry& e = entries[count];
        memcpy(e.name, fdr, 10);
        e.name[10] = '\0';
        for (int k = 9; k >= 0 && e.name[k] == ' '; k--)
        {
          e.name[k] = '\0';
        }
        e.flags        = fdr[0x0C];
        e.totalSectors = (uint16_t(fdr[0x0E]) << 8) | fdr[0x0F];
        e.recLen       = fdr[0x11];
        // Record count is little-endian (TI quirk)
        e.numRecords   = (uint16_t(fdr[0x13]) << 8) | fdr[0x12];
        count++;
      }
      return count;
    }

    // Return total free sectors on the disk by counting zero bits in
    // the sector-allocation bitmap (bytes 0x38..0xFF of sector 0).
    int freeSectors()
    {
      uint8_t s0[SECTOR_SIZE];
      if (!readSector(0, s0)) return 0;
      uint16_t total = (uint16_t(s0[10]) << 8) | s0[11];
      int used = 0;
      for (uint16_t s = 0; s < total; s++)
      {
        uint8_t byte = s0[0x38 + (s >> 3)];
        if (byte & (1 << (s & 7))) used++;
      }
      return (int)total - used;
    }

    // Populate info with file details. name must match the 10-char
    // space-padded form exactly (case sensitive). Returns true on hit.
    bool findFile(const char* name, FileInfo& info)
    {
      char padded[11];
      padName(name, padded);

      uint8_t dir[SECTOR_SIZE];
      if (!readSector(1, dir)) return false;

      for (int i = 0; i < 128; i++)
      {
        uint16_t fdrSec = (uint16_t(dir[i * 2]) << 8) | dir[i * 2 + 1];
        if (fdrSec == 0) break;

        uint8_t fdr[SECTOR_SIZE];
        if (!readSector(fdrSec, fdr)) continue;
        if (memcmp(fdr, padded, 10) != 0) continue;

        parseFdr(fdr, fdrSec, info);
        return true;
      }
      return false;
    }

    // Low-level sector read. Returns false on short read.
    bool readSector(uint16_t sector, uint8_t* buf)
    {
      if (!m_fh) return false;
      if (!m_fh.seek((uint32_t)sector * SECTOR_SIZE)) return false;
      int n = m_fh.read(buf, SECTOR_SIZE);
      return n == SECTOR_SIZE;
    }

    // Low-level sector write. Fails if image is read-only or if the
    // underlying FS write is short.
    //
    // Intentionally does NOT flush() — per-sector flushing on LittleFS
    // commits a flash page each call (~10-100 ms including any block
    // erase), and a single PROGRAM save fires ~10-13 sector writes, so
    // flushing every one stalls the interpreter for seconds. Callers
    // performing a multi-sector logical write (writeRawFile, the
    // DIS/VAR streaming writer, etc.) call flushImage() once at the
    // end. LittleFS retains in-flight data in its RAM cache until then
    // and commits it as a batch — same correctness, no stall.
    bool writeSector(uint16_t sector, const uint8_t* buf)
    {
      if (!m_fh || m_ro) return false;
      if (!m_fh.seek((uint32_t)sector * SECTOR_SIZE)) return false;
      int n = m_fh.write(buf, SECTOR_SIZE);
      return n == SECTOR_SIZE;
    }

    // Commit any buffered sector writes to flash. Call once at the end
    // of a logical save (PROGRAM save, DIS/VAR close, batch copy, etc.)
    // so the on-disk state survives a power-cycle without paying the
    // per-sector flush cost.
    void flushImage()
    {
      if (m_fh && !m_ro) m_fh.flush();
    }

    // Create a fresh, blank V9T9 image at the given path. totalSectors:
    //   360  = SSSD (90 KB)
    //   720  = DSSD (180 KB)
    //   1440 = DSDD (360 KB)
    static bool create(fs::FS& fs, const char* path, const char* volName,
                       int totalSectors)
    {
      if (totalSectors != 360 && totalSectors != 720 &&
          totalSectors != 1440) return false;

      File fh = fs.open(path, "w");
      if (!fh) return false;

      uint8_t s0[SECTOR_SIZE];
      memset(s0, 0, SECTOR_SIZE);

      // Volume name, 10 chars, upper + space padded
      for (int i = 0; i < 10; i++)
      {
        char c = ' ';
        if (volName && i < (int)strlen(volName))
        {
          c = volName[i];
          if (c >= 'a' && c <= 'z') c -= 32;
        }
        s0[i] = (uint8_t)c;
      }
      s0[10] = (totalSectors >> 8) & 0xFF;
      s0[11] = totalSectors & 0xFF;
      s0[13] = 'D'; s0[14] = 'S'; s0[15] = 'K';
      s0[16] = 0;   // not protected

      if (totalSectors == 360)       // SSSD
      {
        s0[12] = 9;  s0[17] = 40; s0[18] = 1; s0[19] = 1;
      }
      else if (totalSectors == 720)  // DSSD
      {
        s0[12] = 9;  s0[17] = 40; s0[18] = 2; s0[19] = 1;
      }
      else                            // DSDD
      {
        s0[12] = 18; s0[17] = 40; s0[18] = 2; s0[19] = 2;
      }

      // Allocation bitmap at 0x38. Mark sectors 0 and 1 used.
      // Everything past totalSectors should be marked used as well so
      // the allocator never picks a non-existent sector.
      s0[0x38] = 0x03;   // bits 0 and 1 set
      for (int s = totalSectors; s < 1600; s++)
      {
        s0[0x38 + (s >> 3)] |= (uint8_t)(1 << (s & 7));
      }

      fh.write(s0, SECTOR_SIZE);

      // Rest of image zero-filled. Chunk size + delay is a compromise:
      // flash erase blocks interrupts which starves the RGB bounce
      // buffer, so we keep each write short and give the panel time to
      // recover. A final full-screen redraw after create() clears any
      // tearing that did happen.
      const int CHUNK = 4 * SECTOR_SIZE;   // 1 KB
      static uint8_t zeroChunk[CHUNK];
      memset(zeroChunk, 0, CHUNK);
      int remaining = (totalSectors - 1) * SECTOR_SIZE;
      while (remaining > 0)
      {
        int n = (remaining >= CHUNK) ? CHUNK : remaining;
        fh.write(zeroChunk, n);
        remaining -= n;
        delay(2);
      }
      fh.flush();
      fh.close();
      return true;
    }

    // --- Allocation bitmap helpers (operate on cached VIB sector 0) ---

    bool loadVib(uint8_t* vibBuf)
    {
      return readSector(0, vibBuf);
    }

    static bool isAlloc(const uint8_t* vib, uint16_t sector)
    {
      return (vib[0x38 + (sector >> 3)] & (1 << (sector & 7))) != 0;
    }

    static void setAlloc(uint8_t* vib, uint16_t sector, bool alloc)
    {
      uint8_t mask = (uint8_t)(1 << (sector & 7));
      if (alloc) vib[0x38 + (sector >> 3)] |= mask;
      else       vib[0x38 + (sector >> 3)] &= ~mask;
    }

    uint16_t findFreeSector(const uint8_t* vib)
    {
      uint16_t total = (uint16_t(vib[10]) << 8) | vib[11];
      for (uint16_t s = 2; s < total; s++)   // skip VIB and directory
      {
        if (!isAlloc(vib, s)) return s;
      }
      return 0;
    }

    // Pad / upper-case a user-supplied name into the 10-char FDR form.
    static void padName(const char* src, char* out11)
    {
      int n = 0;
      while (*src && n < 10)
      {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out11[n++] = c;
      }
      while (n < 10) out11[n++] = ' ';
      out11[10] = '\0';
    }

  private:
    void parseFdr(const uint8_t* fdr, uint16_t fdrSec, FileInfo& info)
    {
      memcpy(info.name, fdr, 10);
      info.name[10] = '\0';
      for (int k = 9; k >= 0 && info.name[k] == ' '; k--)
      {
        info.name[k] = '\0';
      }
      info.fdrSector    = fdrSec;
      info.flags        = fdr[0x0C];
      info.totalSectors = (uint16_t(fdr[0x0E]) << 8) | fdr[0x0F];
      info.eofOffset    = fdr[0x10];
      info.recLen       = fdr[0x11];
      // Little-endian record count
      info.numRecords   = (uint16_t(fdr[0x13]) << 8) | fdr[0x12];

      // Expand cluster chain starting at 0x1C, 3 bytes per cluster.
      info.sectorCount = 0;
      for (int off = 0x1C; off < SECTOR_SIZE &&
             info.sectorCount < 128; off += 3)
      {
        uint8_t b0 = fdr[off];
        uint8_t b1 = fdr[off + 1];
        uint8_t b2 = fdr[off + 2];
        if (b0 == 0 && b1 == 0 && b2 == 0) break;
        uint16_t start = ((uint16_t)(b1 & 0x0F) << 8) | b0;
        uint16_t endOff = ((uint16_t)(b2) << 4) | ((b1 & 0xF0) >> 4);
        for (uint16_t s = 0; s <= endOff - 0 && info.sectorCount < 128; s++)
        {
          // endOff is the last *file-offset* (0-based) covered by this
          // cluster; we iterate until we've filled up to endOff.
          info.sectors[info.sectorCount++] = start + s;
          if (info.sectorCount > endOff) break;
        }
      }
    }

  public:
    // --- DIS/VAR sequential reader ---

    struct DisVarReader
    {
      DskImage* img = nullptr;
      FileInfo  info{};
      int       curLogicalSector = 0;
      int       posInSector = 0;
      uint8_t   buf[SECTOR_SIZE];
      bool      bufLoaded = false;
      bool      eof = false;
      int       recordsRead = 0;
    };

    // Initialize a reader for the given file info.
    bool openDisVarReader(const FileInfo& info, DisVarReader& r)
    {
      r.img = this;
      r.info = info;
      r.curLogicalSector = 0;
      r.posInSector = 0;
      r.bufLoaded = false;
      r.recordsRead = 0;
      r.eof = (info.sectorCount == 0 || info.numRecords == 0);
      return !r.eof;
    }

    // Read one DIS/VAR record into `out`. Returns length (0..254) or
    // -1 at EOF. 0xFF length byte means "sector padding" — advance to
    // the next sector in the chain; real EOF is when we run out of
    // sectors.
    int readDisVarRecord(DisVarReader& r, char* out, int outSize)
    {
      if (r.eof) return -1;
      // Load the current sector if needed
      if (!r.bufLoaded)
      {
        if (r.curLogicalSector >= (int)r.info.sectorCount)
        {
          r.eof = true;
          return -1;
        }
        if (!readSector(r.info.sectors[r.curLogicalSector], r.buf))
        {
          r.eof = true;
          return -1;
        }
        r.bufLoaded = true;
      }

      // Skip sector padding (0xFF length byte or past sector end) until
      // we find a real record or run out of sectors.
      while (true)
      {
        if (r.posInSector < SECTOR_SIZE && r.buf[r.posInSector] != 0xFF)
        {
          break;   // real record length ahead
        }
        r.curLogicalSector++;
        r.posInSector = 0;
        if (r.curLogicalSector >= (int)r.info.sectorCount)
        {
          r.eof = true;
          return -1;
        }
        if (!readSector(r.info.sectors[r.curLogicalSector], r.buf))
        {
          r.eof = true;
          return -1;
        }
      }

      uint8_t len = r.buf[r.posInSector++];
      int copyLen = (len < outSize - 1) ? len : outSize - 1;
      memcpy(out, &r.buf[r.posInSector], copyLen);
      out[copyLen] = '\0';
      r.posInSector += len;
      r.recordsRead++;
      // If we've returned every record the FDR promises, flip EOF now
      // so the caller's next EOF() check succeeds without a spurious
      // empty read.
      if (r.recordsRead >= (int)r.info.numRecords) r.eof = true;
      return (int)len;
    }

    // --- DIS/VAR writer ---

    struct DisVarWriter
    {
      DskImage* img = nullptr;
      char      name[11] = {0};
      uint16_t  fdrSec = 0;
      uint8_t   buf[SECTOR_SIZE];
      int       posInSector = 0;
      int       curLogicalSector = 0;
      int       numRecords = 0;
      uint16_t  sectors[128] = {0};
      uint16_t  sectorCount = 0;
    };

    // Delete an existing file (free its FDR + data sectors, remove
    // directory entry). No-op if the file doesn't exist.
    bool deleteFile(const char* name)
    {
      char padded[11];
      padName(name, padded);

      uint8_t vib[SECTOR_SIZE];
      if (!loadVib(vib)) return false;

      uint8_t dir[SECTOR_SIZE];
      if (!readSector(1, dir)) return false;

      int found = -1;
      FileInfo info;
      for (int i = 0; i < 128; i++)
      {
        uint16_t fdrSec = (uint16_t(dir[i * 2]) << 8) | dir[i * 2 + 1];
        if (fdrSec == 0) break;
        uint8_t fdr[SECTOR_SIZE];
        if (!readSector(fdrSec, fdr)) continue;
        if (memcmp(fdr, padded, 10) != 0) continue;
        parseFdr(fdr, fdrSec, info);
        found = i;
        break;
      }
      if (found < 0) return true;   // not present = ok

      // Free all data sectors + the FDR sector
      for (uint16_t s = 0; s < info.sectorCount; s++)
      {
        setAlloc(vib, info.sectors[s], false);
      }
      setAlloc(vib, info.fdrSector, false);

      // Compact directory: shift remaining entries down by one slot
      for (int i = found; i < 127; i++)
      {
        dir[i * 2]     = dir[(i + 1) * 2];
        dir[i * 2 + 1] = dir[(i + 1) * 2 + 1];
      }
      dir[127 * 2] = 0; dir[127 * 2 + 1] = 0;

      writeSector(1, dir);
      writeSector(0, vib);
      return true;
    }

    // Begin writing a new DIS/VAR file. If a file with this name exists
    // it's deleted first (OUTPUT semantics).
    bool openDisVarWriter(const char* name, DisVarWriter& w)
    {
      if (m_ro) return false;
      if (!deleteFile(name)) return false;

      uint8_t vib[SECTOR_SIZE];
      if (!loadVib(vib)) return false;

      // Allocate FDR sector
      uint16_t fdrSec = findFreeSector(vib);
      if (fdrSec == 0) return false;
      setAlloc(vib, fdrSec, true);

      // Allocate first data sector
      uint16_t dataSec = findFreeSector(vib);
      if (dataSec == 0) return false;
      setAlloc(vib, dataSec, true);

      // Persist bitmap update immediately so a crash leaves no stale free bits
      if (!writeSector(0, vib)) return false;

      // Add FDR pointer to directory, keeping entries sorted alphabetically
      uint8_t dir[SECTOR_SIZE];
      if (!readSector(1, dir)) return false;

      char padded[11];
      padName(name, padded);
      // Find insertion index
      int insertAt = 128;
      for (int i = 0; i < 128; i++)
      {
        uint16_t sec = (uint16_t(dir[i * 2]) << 8) | dir[i * 2 + 1];
        if (sec == 0) { insertAt = i; break; }
        uint8_t fdr[SECTOR_SIZE];
        if (!readSector(sec, fdr)) continue;
        if (memcmp(fdr, padded, 10) > 0)
        {
          insertAt = i;
          break;
        }
      }
      if (insertAt >= 128) return false;   // directory full

      for (int i = 127; i > insertAt; i--)
      {
        dir[i * 2]     = dir[(i - 1) * 2];
        dir[i * 2 + 1] = dir[(i - 1) * 2 + 1];
      }
      dir[insertAt * 2]     = (fdrSec >> 8) & 0xFF;
      dir[insertAt * 2 + 1] = fdrSec & 0xFF;
      if (!writeSector(1, dir)) return false;

      // Initialize writer state
      w.img = this;
      memcpy(w.name, padded, 11);
      w.fdrSec = fdrSec;
      w.posInSector = 0;
      w.curLogicalSector = 0;
      w.numRecords = 0;
      w.sectorCount = 1;
      w.sectors[0] = dataSec;
      memset(w.buf, 0xFF, SECTOR_SIZE);
      return true;
    }

    // Begin writing a new DIS/FIXED file. Same as openDisVarWriter but
    // the sector buffer is zero-padded (FIXED files don't use 0xFF
    // record padding).
    bool openFixedWriter(const char* name, DisVarWriter& w)
    {
      if (!openDisVarWriter(name, w)) return false;
      memset(w.buf, 0, SECTOR_SIZE);
      return true;
    }

    // Close a DIS/FIXED writer. Writes a FIXED-style FDR (no VARIABLE
    // bit, recLen filled in, recordsPerSector filled in).
    bool closeFixedWriter(DisVarWriter& w, int recLen)
    {
      if (!w.img) return false;
      if (recLen <= 0) return false;

      if (!writeSector(w.sectors[w.curLogicalSector], w.buf)) return false;

      uint8_t fdr[SECTOR_SIZE];
      memset(fdr, 0, SECTOR_SIZE);
      memcpy(fdr, w.name, 10);
      fdr[0x0C] = 0x00;   // DISPLAY / FIXED
      fdr[0x0D] = (uint8_t)(SECTOR_SIZE / recLen);   // recs per sector
      fdr[0x0E] = (w.sectorCount >> 8) & 0xFF;
      fdr[0x0F] = w.sectorCount & 0xFF;
      fdr[0x10] = (uint8_t)(w.posInSector & 0xFF);
      fdr[0x11] = (uint8_t)recLen;
      fdr[0x12] = w.numRecords & 0xFF;
      fdr[0x13] = (w.numRecords >> 8) & 0xFF;

      int off = 0x1C;
      uint16_t i = 0;
      while (i < w.sectorCount && off + 3 <= SECTOR_SIZE)
      {
        uint16_t startSec = w.sectors[i];
        uint16_t runLen = 1;
        while (i + runLen < w.sectorCount &&
               w.sectors[i + runLen] == startSec + runLen) runLen++;
        uint16_t endOff = i + runLen - 1;
        fdr[off]     = startSec & 0xFF;
        fdr[off + 1] = ((endOff & 0x0F) << 4) | ((startSec >> 8) & 0x0F);
        fdr[off + 2] = (endOff >> 4) & 0xFF;
        off += 3;
        i += runLen;
      }
      if (!writeSector(w.fdrSec, fdr)) return false;
      w.img = nullptr;
      return true;
    }

    // Write one FIXED-length record. recLen is the file's record length;
    // data is space-padded or truncated to that size. If curRecord is
    // non-null we treat this as RELATIVE access and seek to that record
    // (within the writer's already-allocated sectors); otherwise we
    // append at the writer's current position.
    //
    // Records do not span sectors. Each sector holds floor(256/recLen)
    // records; remaining bytes in the sector are 0x00 padding.
    bool writeFixedRecord(DisVarWriter& w, const char* data, int recLen,
                          long* curRecord)
    {
      if (recLen <= 0 || recLen > 255) return false;
      int recsPerSector = SECTOR_SIZE / recLen;
      if (recsPerSector < 1) return false;

      long targetRec = -1;
      if (curRecord)
      {
        targetRec = *curRecord;
        (*curRecord)++;
      }

      if (targetRec >= 0)
      {
        int targetSector = (int)(targetRec / recsPerSector);
        int slotInSector = (int)(targetRec % recsPerSector);
        // Allocate any new sectors needed to reach targetSector
        while (w.sectorCount <= targetSector)
        {
          // Persist current buffer first
          if (!writeSector(w.sectors[w.curLogicalSector], w.buf)) return false;
          uint8_t vib[SECTOR_SIZE];
          if (!loadVib(vib)) return false;
          uint16_t nextSec = findFreeSector(vib);
          if (nextSec == 0) return false;
          setAlloc(vib, nextSec, true);
          if (!writeSector(0, vib)) return false;
          if (w.sectorCount >= 128) return false;
          w.sectors[w.sectorCount++] = nextSec;
          w.curLogicalSector++;
          memset(w.buf, 0, SECTOR_SIZE);
        }
        // Switch the working buffer to the target sector if needed.
        if (w.curLogicalSector != targetSector)
        {
          if (!writeSector(w.sectors[w.curLogicalSector], w.buf)) return false;
          if (!readSector(w.sectors[targetSector], w.buf)) return false;
          w.curLogicalSector = targetSector;
        }
        int off = slotInSector * recLen;
        int n = (int)strlen(data);
        if (n > recLen) n = recLen;
        memcpy(&w.buf[off], data, n);
        for (int i = n; i < recLen; i++) w.buf[off + i] = ' ';
        if (targetRec + 1 > w.numRecords) w.numRecords = targetRec + 1;
        w.posInSector = (slotInSector + 1) * recLen;
        return true;
      }

      // Sequential append
      if (w.posInSector + recLen > recsPerSector * recLen)
      {
        if (!writeSector(w.sectors[w.curLogicalSector], w.buf)) return false;
        uint8_t vib[SECTOR_SIZE];
        if (!loadVib(vib)) return false;
        uint16_t nextSec = findFreeSector(vib);
        if (nextSec == 0) return false;
        setAlloc(vib, nextSec, true);
        if (!writeSector(0, vib)) return false;
        if (w.sectorCount >= 128) return false;
        w.sectors[w.sectorCount++] = nextSec;
        w.curLogicalSector++;
        w.posInSector = 0;
        memset(w.buf, 0, SECTOR_SIZE);
      }
      int n = (int)strlen(data);
      if (n > recLen) n = recLen;
      memcpy(&w.buf[w.posInSector], data, n);
      for (int i = n; i < recLen; i++) w.buf[w.posInSector + i] = ' ';
      w.posInSector += recLen;
      w.numRecords++;
      return true;
    }

    // Read one FIXED-length record. Returns recLen on success or -1 at
    // EOF. If curRecord is non-null, treat as RELATIVE: read record
    // *curRecord and post-increment.
    int readFixedRecord(DisVarReader& r, char* out, int outSize, int recLen,
                        long* curRecord)
    {
      if (recLen <= 0 || recLen > 255) return -1;
      int recsPerSector = SECTOR_SIZE / recLen;
      if (recsPerSector < 1) return -1;

      long targetRec;
      if (curRecord)
      {
        targetRec = *curRecord;
        (*curRecord)++;
      }
      else
      {
        targetRec = (long)r.recordsRead;
      }

      if (targetRec >= (long)r.info.numRecords)
      {
        r.eof = true;
        return -1;
      }
      int targetSector = (int)(targetRec / recsPerSector);
      int slotInSector = (int)(targetRec % recsPerSector);
      if (targetSector >= (int)r.info.sectorCount)
      {
        r.eof = true;
        return -1;
      }
      if (!r.bufLoaded || r.curLogicalSector != targetSector)
      {
        if (!readSector(r.info.sectors[targetSector], r.buf))
        {
          r.eof = true;
          return -1;
        }
        r.bufLoaded = true;
        r.curLogicalSector = targetSector;
      }
      int copyLen = (recLen < outSize - 1) ? recLen : outSize - 1;
      memcpy(out, &r.buf[slotInSector * recLen], copyLen);
      out[copyLen] = '\0';
      r.recordsRead++;
      if (r.recordsRead >= (int)r.info.numRecords) r.eof = true;
      return recLen;
    }

    bool writeDisVarRecord(DisVarWriter& w, const char* data)
    {
      int len = (int)strlen(data);
      if (len > 254) len = 254;

      // Records never span sectors. If the record won't fit, pad the
      // tail with 0xFF and allocate a fresh sector.
      if (w.posInSector + 1 + len > SECTOR_SIZE)
      {
        // Flush current sector (tail is already 0xFF from initial fill)
        if (!writeSector(w.sectors[w.curLogicalSector], w.buf)) return false;
        // Allocate next data sector
        uint8_t vib[SECTOR_SIZE];
        if (!loadVib(vib)) return false;
        uint16_t nextSec = findFreeSector(vib);
        if (nextSec == 0) return false;
        setAlloc(vib, nextSec, true);
        if (!writeSector(0, vib)) return false;
        if (w.sectorCount >= 128) return false;
        w.sectors[w.sectorCount++] = nextSec;
        w.curLogicalSector++;
        w.posInSector = 0;
        memset(w.buf, 0xFF, SECTOR_SIZE);
      }

      w.buf[w.posInSector++] = (uint8_t)len;
      memcpy(&w.buf[w.posInSector], data, len);
      w.posInSector += len;
      w.numRecords++;
      return true;
    }

    // --- Raw (PROGRAM / INT FIX / etc.) file I/O ---

    // Read a whole file's bytes into buf. Returns size read (>=0) or -1
    // on error. Caller sizes the buffer.
    int readRawFile(const char* name, uint8_t* buf, int bufSize)
    {
      FileInfo info;
      if (!findFile(name, info)) return -1;
      int byteCount = info.sectorCount * SECTOR_SIZE;
      if (info.eofOffset != 0 && info.sectorCount > 0)
      {
        byteCount = (info.sectorCount - 1) * SECTOR_SIZE + info.eofOffset;
      }
      if (byteCount > bufSize) return -1;
      int off = 0;
      for (uint16_t i = 0; i < info.sectorCount && off < byteCount; i++)
      {
        uint8_t sbuf[SECTOR_SIZE];
        if (!readSector(info.sectors[i], sbuf)) return -1;
        int want = byteCount - off;
        if (want > SECTOR_SIZE) want = SECTOR_SIZE;
        memcpy(&buf[off], sbuf, want);
        off += want;
      }
      return byteCount;
    }

    // Write buf as a new file of the given type flags (e.g. 0x01 for
    // PROGRAM). Overwrites any existing file of this name.
    bool writeRawFile(const char* name, const uint8_t* buf, int size,
                      uint8_t typeFlags)
    {
      if (m_ro) return false;
      if (!deleteFile(name)) return false;

      uint8_t vib[SECTOR_SIZE];
      if (!loadVib(vib)) return false;

      uint16_t fdrSec = findFreeSector(vib);
      if (fdrSec == 0) return false;
      setAlloc(vib, fdrSec, true);
      if (!writeSector(0, vib)) return false;

      int sectorsNeeded = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
      if (sectorsNeeded == 0) sectorsNeeded = 1;
      uint16_t dataSecs[128];
      if (sectorsNeeded > 128) return false;

      if (!loadVib(vib)) return false;
      for (int i = 0; i < sectorsNeeded; i++)
      {
        uint16_t s = findFreeSector(vib);
        if (s == 0) return false;
        setAlloc(vib, s, true);
        dataSecs[i] = s;
      }
      if (!writeSector(0, vib)) return false;

      uint8_t secBuf[SECTOR_SIZE];
      int written = 0;
      for (int i = 0; i < sectorsNeeded; i++)
      {
        memset(secBuf, 0, SECTOR_SIZE);
        int n = size - written;
        if (n > SECTOR_SIZE) n = SECTOR_SIZE;
        if (n > 0) memcpy(secBuf, &buf[written], n);
        if (!writeSector(dataSecs[i], secBuf)) return false;
        written += n;
      }

      // Write FDR
      uint8_t fdr[SECTOR_SIZE];
      memset(fdr, 0, SECTOR_SIZE);
      char padded[11];
      padName(name, padded);
      memcpy(fdr, padded, 10);
      fdr[0x0C] = typeFlags;
      fdr[0x0E] = (sectorsNeeded >> 8) & 0xFF;
      fdr[0x0F] = sectorsNeeded & 0xFF;
      fdr[0x10] = (uint8_t)(size % SECTOR_SIZE);
      fdr[0x11] = 0;   // no record length for PROGRAM
      fdr[0x12] = 0; fdr[0x13] = 0;

      // Build cluster chain (consolidate contiguous runs)
      int off = 0x1C;
      int i = 0;
      while (i < sectorsNeeded && off + 3 <= SECTOR_SIZE)
      {
        uint16_t start = dataSecs[i];
        int runLen = 1;
        while (i + runLen < sectorsNeeded &&
               dataSecs[i + runLen] == start + runLen) runLen++;
        uint16_t endOff = i + runLen - 1;
        fdr[off]     = start & 0xFF;
        fdr[off + 1] = ((endOff & 0x0F) << 4) | ((start >> 8) & 0x0F);
        fdr[off + 2] = (endOff >> 4) & 0xFF;
        off += 3;
        i += runLen;
      }
      if (!writeSector(fdrSec, fdr)) return false;

      // Add directory entry (alphabetic insertion)
      uint8_t dir[SECTOR_SIZE];
      if (!readSector(1, dir)) return false;
      uint8_t other[SECTOR_SIZE];
      int insertAt = 128;
      for (int k = 0; k < 128; k++)
      {
        uint16_t sec = ((uint16_t)dir[k * 2] << 8) | dir[k * 2 + 1];
        if (sec == 0) { insertAt = k; break; }
        if (!readSector(sec, other)) continue;
        if (memcmp(other, padded, 10) > 0) { insertAt = k; break; }
      }
      if (insertAt >= 128) return false;
      for (int k = 127; k > insertAt; k--)
      {
        dir[k * 2]     = dir[(k - 1) * 2];
        dir[k * 2 + 1] = dir[(k - 1) * 2 + 1];
      }
      dir[insertAt * 2]     = (fdrSec >> 8) & 0xFF;
      dir[insertAt * 2 + 1] = fdrSec & 0xFF;
      if (!writeSector(1, dir)) return false;
      flushImage();   // commit the whole save as one flash transaction
      return true;
    }

    bool closeDisVarWriter(DisVarWriter& w)
    {
      if (!w.img) return false;

      // Flush final partial sector (tail already 0xFF-padded)
      if (!writeSector(w.sectors[w.curLogicalSector], w.buf)) return false;

      // Build FDR
      uint8_t fdr[SECTOR_SIZE];
      memset(fdr, 0, SECTOR_SIZE);
      memcpy(fdr, w.name, 10);
      fdr[0x0C] = 0x40;   // DISPLAY / VARIABLE
      fdr[0x0E] = (w.sectorCount >> 8) & 0xFF;
      fdr[0x0F] = w.sectorCount & 0xFF;
      fdr[0x10] = (uint8_t)(w.posInSector & 0xFF);   // EOF offset
      fdr[0x11] = 80;   // nominal record length
      fdr[0x12] = w.numRecords & 0xFF;         // LE quirk
      fdr[0x13] = (w.numRecords >> 8) & 0xFF;

      // Cluster chain — consolidate contiguous runs into single entries
      int off = 0x1C;
      uint16_t i = 0;
      while (i < w.sectorCount && off + 3 <= SECTOR_SIZE)
      {
        uint16_t startSec = w.sectors[i];
        uint16_t runLen = 1;
        while (i + runLen < w.sectorCount &&
               w.sectors[i + runLen] == startSec + runLen) runLen++;
        uint16_t endOff = i + runLen - 1;
        fdr[off]     = startSec & 0xFF;
        fdr[off + 1] = ((endOff & 0x0F) << 4) | ((startSec >> 8) & 0x0F);
        fdr[off + 2] = (endOff >> 4) & 0xFF;
        off += 3;
        i += runLen;
      }

      if (!writeSector(w.fdrSec, fdr)) return false;
      flushImage();   // commit the whole DIS/VAR write as one transaction
      w.img = nullptr;
      return true;
    }

  private:
    File     m_fh;
    bool     m_ro = true;
    Vib      m_vib{};
    fs::FS*  m_fs = nullptr;
    char     m_path[48] = {0};
  };
}

#endif // DSK_IMAGE_H
