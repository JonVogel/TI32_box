// program_io.cpp — see program_io.h for design notes.

#include "program_io.h"

namespace progio
{
  // Serialize the current program into a V9T9 PROGRAM-format byte
  // stream. Layout:
  //   bytes 0..7   header: cksum, LNT top, LNT bot, prog top (BE16)
  //   bytes 8..    LNT table (4 bytes per line, descending line num)
  //   bytes ...    program text, each line = [len][toks...][0x00]
  //
  // The cksum is LNT_top XOR LNT_bot XOR prog_top (just enough for
  // sanity-checking; real TI's PROGRAM-file checksum is non-functional
  // on most readers and most emulators just use the layout pointers).
  int saveProgramBytes(const LineSource& src, uint8_t* buf, int bufSize)
  {
    if (!src.count || !src.get) return -1;
    int n = src.count();
    if (n <= 0) return 0;

    // First pass: compute sizes so we can lay out pointers.
    // TI line format = [length][tokens...][0x00], so each line
    // consumes 2 + tokenCount bytes in the text region.
    int textSize = 0;
    for (int i = 0; i < n; i++)
    {
      int lineNum = 0;
      const uint8_t* toks = nullptr;
      int len = 0;
      src.get(i, &lineNum, &toks, &len);
      // Caller might supply a TOK_EOL-terminated stream; strip it.
      // We don't know the value of TOK_EOL from the library side, but
      // the de-facto convention is that the caller passes raw tokens
      // without the terminator. If the caller still includes it, the
      // resulting file is one byte longer per line — not corrupt, just
      // slightly wrong. Callers should pass len excluding any TOK_EOL.
      if (len < 0) len = 0;
      textSize += 2 + len;
    }
    int lntSize = n * 4;
    int total = 8 + textSize + lntSize;
    if (total > bufSize) return -1;

    // Layout the VDP file-image. LNT starts at VDP 0x0008 (= file
    // offset 8 after the header), program text follows.
    uint16_t lntBot   = 0x0008;
    uint16_t lntTop   = lntBot + lntSize - 1;
    uint16_t textBase = lntTop + 1;
    uint16_t progTop  = textBase + textSize - 1;

    // Header (big-endian 16-bit fields).
    uint16_t cksum = lntTop ^ lntBot ^ progTop;
    buf[0] = (cksum   >> 8) & 0xFF; buf[1] = cksum   & 0xFF;
    buf[2] = (lntTop  >> 8) & 0xFF; buf[3] = lntTop  & 0xFF;
    buf[4] = (lntBot  >> 8) & 0xFF; buf[5] = lntBot  & 0xFF;
    buf[6] = (progTop >> 8) & 0xFF; buf[7] = progTop & 0xFF;

    // Write program text. Track each line's VDP pointer (the address
    // of its FIRST TOKEN, one byte past the length byte) so the LNT
    // can reference it correctly.
    int textFileOff = 8 + lntSize;
    uint16_t textWrite = textBase;
    // Stack allocation isn't safe (n unknown); use a small heap buffer.
    uint16_t* linePtrs = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)n);
    if (!linePtrs) return -1;

    int off = textFileOff;
    for (int i = 0; i < n; i++)
    {
      int lineNum = 0;
      const uint8_t* toks = nullptr;
      int len = 0;
      src.get(i, &lineNum, &toks, &len);
      if (len < 0) len = 0;
      buf[off++] = (uint8_t)(len + 1);   // length byte counts the 0x00
      linePtrs[i] = textWrite + 1;       // LNT points past the length
      if (len > 0 && toks)
      {
        memcpy(&buf[off], toks, (size_t)len);
        off += len;
      }
      buf[off++] = 0x00;                 // TI-style line terminator
      textWrite += 2 + len;
    }

    // Write LNT at file offset 8, sorted descending by line number to
    // match TI convention (LNT grows downward in VDP as lines added).
    int lntOff = 8;
    for (int i = n - 1; i >= 0; i--)
    {
      int lineNum = 0;
      const uint8_t* toks = nullptr;
      int len = 0;
      src.get(i, &lineNum, &toks, &len);
      buf[lntOff++] = (uint8_t)((lineNum >> 8) & 0xFF);
      buf[lntOff++] = (uint8_t)(lineNum & 0xFF);
      buf[lntOff++] = (uint8_t)((linePtrs[i] >> 8) & 0xFF);
      buf[lntOff++] = (uint8_t)(linePtrs[i] & 0xFF);
    }

    free(linePtrs);
    return total;
  }

  // Parse the inverse of saveProgramBytes. Walks the LNT, looks up
  // each line's text region, and emits one dst.store() per line.
  bool loadProgramBytes(const uint8_t* buf, int size,
                        const LineSink& dst, int maxTokens)
  {
    if (size < 12 || !dst.clear || !dst.store) return false;

    uint16_t lntTop  = ((uint16_t)buf[2] << 8) | buf[3];
    uint16_t lntBot  = ((uint16_t)buf[4] << 8) | buf[5];
    uint16_t progTop = ((uint16_t)buf[6] << 8) | buf[7];
    if (lntBot > lntTop || progTop < lntTop) return false;

    // VDP-to-file-offset map: VDP addr X lives at file offset 8 + (X - lntBot).
    auto vdp2file = [&](uint16_t addr) -> int
    {
      if (addr < lntBot || addr > progTop) return -1;
      int off = 8 + (int)(addr - lntBot);
      return (off < size) ? off : -1;
    };

    dst.clear();

    int stored = 0;
    for (uint16_t ent = lntBot; ent + 3 <= lntTop; ent += 4)
    {
      int off = vdp2file(ent);
      if (off < 0 || off + 4 > size) break;
      uint16_t lineNum = ((uint16_t)buf[off] << 8) | buf[off + 1];
      uint16_t tokPtr  = ((uint16_t)buf[off + 2] << 8) | buf[off + 3];

      // TI stores each line as: [length byte][tokens...][0x00].
      // The LNT pointer is to the FIRST TOKEN (one byte past the
      // length). So length is at tokPtr - 1.
      int tokOff = vdp2file(tokPtr);
      if (tokOff < 1 || tokOff >= size) continue;
      uint8_t len = buf[tokOff - 1];
      if (len == 0 || tokOff + len > size) continue;

      // Some writers store length including the trailing 0x00; others
      // don't. Strip a trailing 0x00 so our caller gets a consistent
      // raw-tokens view.
      int copyLen = len;
      if (copyLen > 0 && buf[tokOff + copyLen - 1] == 0x00) copyLen--;

      if (copyLen > maxTokens) continue;

      dst.store((int)lineNum, &buf[tokOff], copyLen);
      stored++;
    }
    return stored > 0;
  }
}
