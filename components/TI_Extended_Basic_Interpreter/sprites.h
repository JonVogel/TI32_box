/*
 * TI-99/4A sprite layer (software-emulated, no hardware VDP).
 *
 * The real TMS9918 supports 32 sprites in two sizes (8x8 or 16x16)
 * plus a "magnify" bit that doubles each sprite pixel to 2x2. TI
 * Extended BASIC exposes 28 of them (1..28) via CALL SPRITE / MOTION
 * / POSITION / COINC / DISTANCE / DELSPRITE / MAGNIFY / PATTERN.
 *
 * Coordinate system:
 *   - TI screen is 192 rows × 256 columns of pixels (each TI pixel
 *     is rendered as a 2x2 block on our 800×480 panel).
 *   - Sprite position is the top-left of the sprite in TI-pixel
 *     coordinates, 1-based (i.e. row=1 is the top row).
 *   - Each TI pixel maps to physical (y = (row-1)*2 + DISPLAY_Y_OFFSET,
 *     x = (col-1)*2 + DISPLAY_X_OFFSET).
 *
 * This layer draws sprites directly on the display (layered over the
 * 32×24 character grid). "Transparent" sprite pixels (pattern bit 0)
 * leave the character grid visible. When a sprite moves or is
 * deleted, the character cells it overlapped are repainted via
 * drawCell() to restore what was underneath.
 */

#ifndef SPRITES_H
#define SPRITES_H

#include <stdint.h>
#include <string.h>

namespace sprites
{
  static const int MAX_SPRITES = 28;

  enum MagnifyMode : uint8_t
  {
    MAG_1 = 1,   // 8x8 unmagnified
    MAG_2 = 2,   // 8x8 doubled → 16x16 (each source pixel → 2x2)
    MAG_3 = 3,   // 16x16 unmagnified (four 8x8 chars)
    MAG_4 = 4    // 16x16 doubled → 32x32
  };

  struct Sprite
  {
    bool    active      = false;
    uint8_t charCode    = 0;      // base pattern (32..143)
    uint8_t colorIdx    = 16;     // palette index (1..16)
    int16_t row         = 1;      // live TI-pixel row, top edge (1-based)
    int16_t col         = 1;      // live TI-pixel col, left edge (1-based)
    int16_t rowVel      = 0;      // velocity, -128..127
    int16_t colVel      = 0;
    uint8_t magnify     = MAG_1;  // global in real TI; per-sprite here for now
    // Sub-pixel accumulators so slow velocities still produce smooth
    // motion. Effective pixel motion = vel / 8 per 1/60 s frame.
    int32_t subRow      = 0;
    int32_t subCol      = 0;
    // Snapshot captured at the end of each spriteTick (the virtual
    // vsync). CALL POSITION / COINC / DISTANCE all read from these
    // instead of the live row/col so a single BASIC iteration sees
    // a coherent frame: positions queried at line 60 and used on
    // line 130 won't have shifted underfoot if a tick fired in the
    // middle.
    int16_t snapRow     = 1;
    int16_t snapCol     = 1;
    uint8_t snapMagnify = MAG_1;
  };

  inline Sprite g_sprites[MAX_SPRITES + 1];    // 1..MAX_SPRITES
  inline uint8_t g_magnify = MAG_1;            // CALL MAGNIFY sets this

  inline void clearAll()
  {
    for (int i = 0; i <= MAX_SPRITES; i++)
    {
      g_sprites[i] = Sprite();
    }
    g_magnify = MAG_1;
  }

  // Return true if the sprite number is in range.
  inline bool validSlot(int n) { return n >= 1 && n <= MAX_SPRITES; }

  // Returns the source-pattern dimension in TI pixels (before scale):
  // 8 for MAG_1 / MAG_2, 16 for MAG_3 / MAG_4.
  inline int bodySize(uint8_t magnify)
  {
    return (magnify == MAG_3 || magnify == MAG_4) ? 16 : 8;
  }

  // On-screen footprint in TI pixels (body × scale). Use this for
  // COINC bounding-box checks, not bodySize().
  inline int footprint(uint8_t magnify)
  {
    int body  = bodySize(magnify);
    int scale = (magnify == MAG_2 || magnify == MAG_4) ? 2 : 1;
    return body * scale;
  }

  // True if the pixel bit (1 or 0) at sprite-local row sr, col sc is on.
  // Reads from up to four 8x8 patterns (charCode .. charCode+3) for
  // 16x16 sprites. Caller passes a charPatterns[256][8] table.
  inline bool pixelOn(uint8_t charCode, uint8_t magnify, int sr, int sc,
                      const uint8_t charPatterns[][8])
  {
    int body = bodySize(magnify);
    if (sr < 0 || sr >= body || sc < 0 || sc >= body) return false;
    int cellR = 0, cellC = 0, subR = sr, subC = sc;
    if (body == 16)
    {
      cellR = sr / 8;
      cellC = sc / 8;
      subR  = sr % 8;
      subC  = sc % 8;
    }
    // TI 16x16 char layout inside one sprite:
    //   cellR=0, cellC=0 → base
    //   cellR=1, cellC=0 → base+1
    //   cellR=0, cellC=1 → base+2
    //   cellR=1, cellC=1 → base+3
    uint8_t code = charCode + cellR + cellC * 2;
    uint8_t bits = charPatterns[code][subR];
    return (bits & (0x80 >> subC)) != 0;
  }
}

#endif // SPRITES_H
