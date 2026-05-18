// TI_Speech/tms5220_coeffs.h
//
// TMS5220 LPC synthesis coefficient tables — energy / pitch / k1..k10 /
// chirp / interpolation. Extracted from MAME's src/devices/sound/
// tms5110r.hxx (the original ships tables for every TI51xx/52xx chip
// variant; we keep only the TMS5220 subset since that's what the
// TI-99/4A Speech Synthesizer cartridge used).
//
// SPDX-License-Identifier: BSD-3-Clause
// Original copyright: Frank Palazzolo, Couriersud, Jonathan Gevaryahu
// Source: TMS5220/5220C entry, verified to match decap of TMS5220NL
//         and TMS5220CNL by digshadow (April 2013).
//
// Notation:
//   energy[16]: 4-bit index → 16-bit energy magnitude. Index 0 = silence,
//               index 15 = stop frame.
//   pitch[64]:  6-bit index → pitch period in samples. Index 0 = unvoiced.
//   k1..k4:     5-bit index → 10-bit signed reflection coefficient.
//   k5..k10:    larger entries with fewer bits (see kbits[] below).
//   chirp[52]:  fixed excitation waveform for voiced frames (8-bit signed).
//   interp:     shift amounts for the 7-step interpolator between frames.
//
// All tables PROGMEM-resident: total ~250 bytes, kept out of SRAM.

#pragma once

#include <stdint.h>
#include <pgmspace.h>

namespace tms5220
{

static constexpr int NUM_K        = 10;
static constexpr int ENERGY_BITS  = 4;
static constexpr int PITCH_BITS   = 6;
static constexpr uint8_t KBITS[NUM_K] = { 5, 5, 4, 4, 4, 4, 4, 3, 3, 3 };

// Energy: TI_028X_LATER_ENERGY (chip rev D/F, same as 5200/5220)
static const uint16_t ENERGY_TABLE[16] PROGMEM = {
  0, 1, 2, 3, 4, 6, 8, 11,
  16, 23, 33, 47, 63, 85, 114, 0
};

// Pitch: TI_5220_PITCH (specific to 5220, different from 5110/5200)
static const uint16_t PITCH_TABLE[64] PROGMEM = {
    0,  15,  16,  17,  18,  19,  20,  21,
   22,  23,  24,  25,  26,  27,  28,  29,
   30,  31,  32,  33,  34,  35,  36,  37,
   38,  39,  40,  41,  42,  44,  46,  48,
   50,  52,  53,  56,  58,  60,  62,  65,
   68,  70,  72,  76,  78,  80,  84,  86,
   91,  94,  98, 101, 105, 109, 114, 118,
  122, 127, 132, 137, 142, 148, 153, 159
};

// K1..K10 reflection coefficients (TI_5110_5220_LPC — identical to TMS5110A)
// Stored as int16_t for PROGMEM ease; values fit in 10 bits signed.
static const int16_t K1_TABLE[32] PROGMEM = {
  -501, -498, -497, -495, -493, -491, -488, -482,
  -478, -474, -469, -464, -459, -452, -445, -437,
  -412, -380, -339, -288, -227, -158,  -81,   -1,
    80,  157,  226,  287,  337,  379,  411,  436
};
static const int16_t K2_TABLE[32] PROGMEM = {
  -328, -303, -274, -244, -211, -175, -138,  -99,
   -59,  -18,   24,   64,  105,  143,  180,  215,
   248,  278,  306,  331,  354,  374,  392,  408,
   422,  435,  445,  455,  463,  470,  476,  506
};
static const int16_t K3_TABLE[16] PROGMEM = {
  -441, -387, -333, -279, -225, -171, -117,  -63,
    -9,   45,   98,  152,  206,  260,  314,  368
};
static const int16_t K4_TABLE[16] PROGMEM = {
  -328, -273, -217, -161, -106,  -50,    5,   61,
   116,  172,  228,  283,  339,  394,  450,  506
};
static const int16_t K5_TABLE[16] PROGMEM = {
  -328, -282, -235, -189, -142,  -96,  -50,   -3,
    43,   90,  136,  182,  229,  275,  322,  368
};
static const int16_t K6_TABLE[16] PROGMEM = {
  -256, -212, -168, -123,  -79,  -35,   10,   54,
    98,  143,  187,  232,  276,  320,  365,  409
};
static const int16_t K7_TABLE[16] PROGMEM = {
  -308, -260, -212, -164, -117,  -69,  -21,   27,
    75,  122,  170,  218,  266,  314,  361,  409
};
static const int16_t K8_TABLE[8] PROGMEM = {
  -256, -161,  -66,   29,  124,  219,  314,  409
};
static const int16_t K9_TABLE[8] PROGMEM = {
  -256, -176,  -96,  -15,   65,  146,  226,  307
};
static const int16_t K10_TABLE[8] PROGMEM = {
  -205, -132,  -59,   14,   87,  160,  234,  307
};

// Chirp ROM: TI_LATER_CHIRP (8-bit signed excitation for voiced frames).
// 52 entries; addresses past 51 saturate at chirp[51].
static const int8_t CHIRP_TABLE[52] PROGMEM = {
  (int8_t)0x00, (int8_t)0x03, (int8_t)0x0f, (int8_t)0x28,
  (int8_t)0x4c, (int8_t)0x6c, (int8_t)0x71, (int8_t)0x50,
  (int8_t)0x25, (int8_t)0x26, (int8_t)0x4c, (int8_t)0x44,
  (int8_t)0x1a, (int8_t)0x32, (int8_t)0x3b, (int8_t)0x13,
  (int8_t)0x37, (int8_t)0x1a, (int8_t)0x25, (int8_t)0x1f,
  (int8_t)0x1d, (int8_t)0x00, (int8_t)0x00, (int8_t)0x00,
  (int8_t)0x00, (int8_t)0x00, (int8_t)0x00, (int8_t)0x00,
  (int8_t)0x00, (int8_t)0x00, (int8_t)0x00, (int8_t)0x00,
  (int8_t)0x00, (int8_t)0x00, (int8_t)0x00, (int8_t)0x00,
  (int8_t)0x00, (int8_t)0x00, (int8_t)0x00, (int8_t)0x00,
  (int8_t)0x00, (int8_t)0x00, (int8_t)0x00, (int8_t)0x00,
  (int8_t)0x00, (int8_t)0x00, (int8_t)0x00, (int8_t)0x00,
  (int8_t)0x00, (int8_t)0x00, (int8_t)0x00, (int8_t)0x00
};

// Interpolation right-shift amounts per IP step: TI_INTERP.
// IP=0 means "frame just loaded" (handled specially in process()), the
// rest are the shift amounts used in the interpolation formula
// (delta = (target - current) >> shift).
static const int8_t INTERP_COEFF[8] PROGMEM = { 0, 3, 3, 3, 2, 2, 1, 1 };

// Reload table: IP counter reload value at frame start. The 5220 (non-C)
// always uses index 0 → 0, meaning IP runs 0..7 (8 interpolation steps,
// the standard 25 ms frame at 8 kHz). Kept as an array for parity with
// the MAME source so port edits remain easy to track.
static const uint8_t RELOAD_TABLE[4] = { 0, 2, 4, 6 };

// Accessor wrappers around pgm_read_* so the synth core stays
// PROGMEM-agnostic and we can swap to plain RAM during host-side testing.
static inline uint16_t energyAt(uint8_t i)
{
  return pgm_read_word(&ENERGY_TABLE[i & 0xF]);
}
static inline uint16_t pitchAt(uint8_t i)
{
  return pgm_read_word(&PITCH_TABLE[i & 0x3F]);
}
static inline int16_t kAt(int row, uint8_t idx)
{
  // The k-coefficient table is ragged: rows 0..1 have 32 entries (5 bits),
  // rows 2..6 have 16 (4 bits), rows 7..9 have 8 (3 bits). The caller
  // masks idx to KBITS[row] worth of bits in readBits(), so we just index.
  switch (row)
  {
    case 0: return (int16_t)pgm_read_word(&K1_TABLE[idx & 0x1F]);
    case 1: return (int16_t)pgm_read_word(&K2_TABLE[idx & 0x1F]);
    case 2: return (int16_t)pgm_read_word(&K3_TABLE[idx & 0x0F]);
    case 3: return (int16_t)pgm_read_word(&K4_TABLE[idx & 0x0F]);
    case 4: return (int16_t)pgm_read_word(&K5_TABLE[idx & 0x0F]);
    case 5: return (int16_t)pgm_read_word(&K6_TABLE[idx & 0x0F]);
    case 6: return (int16_t)pgm_read_word(&K7_TABLE[idx & 0x0F]);
    case 7: return (int16_t)pgm_read_word(&K8_TABLE[idx & 0x07]);
    case 8: return (int16_t)pgm_read_word(&K9_TABLE[idx & 0x07]);
    case 9: return (int16_t)pgm_read_word(&K10_TABLE[idx & 0x07]);
    default: return 0;
  }
}
static inline int8_t chirpAt(uint8_t i)
{
  uint8_t idx = (i > 51) ? 51 : i;
  return (int8_t)pgm_read_byte(&CHIRP_TABLE[idx]);
}
static inline int8_t interpAt(uint8_t ip)
{
  return (int8_t)pgm_read_byte(&INTERP_COEFF[ip & 0x07]);
}

} // namespace tms5220
