// TI_Speech/tms5220.cpp — see tms5220.h for license header (BSD-3-Clause,
// original MAME copyright holders preserved).
//
// Port notes:
//   - The chip's command FIFO (used in MAME for SPEAK / SPEAK-EXTERNAL /
//     LOAD-ADDRESS / READ-BYTE commands) is replaced by a plain byte FIFO
//     that we always treat as "speak external" data — the host has
//     already done the spchrom.bin vocabulary lookup and is just streaming
//     LPC bytes.
//   - We always operate in "non-C" 5220 mode: m_c_variant_rate = 0,
//     m_subc_reload = 1 (normal speech rate, not SPKSLOW), m_digital_select
//     = 0 (analog SPK output → clip_analog path).
//   - Interpolation uses the canonical (non-perfect) algorithm: parameters
//     update only on subcycle 2 of each PC slot. The "perfect interpolation
//     hack" path in MAME is not ported.
//   - TMS5220 always feeds 8 kHz mono. Caller resamples upward.

#include "tms5220.h"
#include "tms5220_coeffs.h"

#if TMS5220_DEBUG
#include <Arduino.h>
#endif

namespace tms5220
{

Synth::Synth()
{
  reset();
}

void Synth::reset()
{
  m_fifo_head = m_fifo_tail = m_fifo_count = 0;
  m_fifo_bits_taken = 0;
  m_buffer_empty = true;

  m_SPEN = m_TALK = m_TALKD = false;
  m_OLDE = true;      // start-of-speech = old frame was silence
  m_OLDP = true;      // and unvoiced
  m_inhibit = false;
  m_uv_zpar = m_zpar = false;
  m_pitch_zero = false;

  m_new_frame_energy_idx = 0;
  m_new_frame_pitch_idx  = 0;
  for (int i = 0; i < 10; i++) m_new_frame_k_idx[i] = 0;

  m_current_energy = 0;
  m_current_pitch  = 0;
  for (int i = 0; i < 10; i++) m_current_k[i] = 0;
  m_previous_energy = 0;

  // Per MAME: PC starts at 0, IP at 0, subcycle at the reload value
  // (which is 1 for normal 5220 — 5100 SPKSLOW uses 0 to add an extra
  // A' cycle). We don't emulate SPKSLOW so subc_reload is implicit.
  m_PC = 0;
  m_IP = 0;
  m_subcycle = 1;
  m_pitch_count = 0;

  // RNG seed: MAME uses 0x1FFF as documented seed (per US patent 4,331,836).
  m_RNG = 0x1FFF;
  m_excitation_data = 0;

  for (int i = 0; i < 11; i++) m_u[i] = 0;
  for (int i = 0; i < 10; i++) m_x[i] = 0;
}

size_t Synth::writeBytes(const uint8_t* bytes, size_t count)
{
  size_t written = 0;
  while (count-- && m_fifo_count < INPUT_FIFO_BYTES)
  {
    m_fifo[m_fifo_tail] = *bytes++;
    m_fifo_tail = (m_fifo_tail + 1) % INPUT_FIFO_BYTES;
    m_fifo_count++;
    written++;
  }
  m_buffer_empty = (m_fifo_count == 0);

  // Implicit SPEAK-EXTERNAL: ensure the chip will consume the new bytes.
  // Real hardware has a SPEAK-EXTERNAL command for this; we re-arm SPEN
  // whenever bytes arrive on an idle-or-shutting-down chip.
  //
  // Two cases matter:
  //   (a) Fully idle (TALK + TALKD + SPEN all false). Reset PC/IP/sub
  //       so the next frame parse fires cleanly at the next IP=0 PC=12
  //       sub=1 boundary.
  //   (b) Shutting down (TALKD still true, ramping energy to zero from
  //       a previous STOP frame). DON'T reset counters — those are
  //       advancing through the ramp and resetting would desync the
  //       interpolation. Just set SPEN so when the chip reaches the
  //       next frame boundary it parses the new word's first frame.
  // Without case (b), a back-to-back CALL SAY where the second word's
  // bytes arrive during the first word's ~26ms shutdown ramp would
  // leave those bytes orphaned in the FIFO forever (isTalking stuck
  // true via m_fifo_count > 0).
  if (!m_SPEN)
  {
    m_SPEN = true;
    if (!m_TALK && !m_TALKD)
    {
      m_PC = 0;
      m_IP = 0;
      m_subcycle = 1;
    }
  }
  return written;
}

// Mirror MAME's read_bits() for the FIFO/DDIS path only. Returns the next
// `count` bits MSB-first from the byte at FIFO head. If the FIFO underflows
// mid-frame the read returns whatever was available (the missing bits are
// effectively zero, which on a real chip would manifest as a stop frame
// from the empty-buffer latch — handled by isTalking() going false).
int Synth::readBits(int count)
{
  int val = 0;
  while (count--)
  {
    if (m_fifo_count == 0)
    {
      // FIFO empty: shift in 0s. The caller will notice a 0 energy index
      // (silence) or a stop pattern on its own.
      val <<= 1;
      m_buffer_empty = true;
      continue;
    }
    // TMS5220 reads LPC bytes MSB-first within each byte (per US patent
    // 4,331,836 and dumpspch.cpp reference). The original LSB-first read
    // here meant every nibble in every frame came out bit-reversed,
    // producing garbled-but-speech-shaped noise instead of intelligible
    // words.
    val = (val << 1) |
          ((m_fifo[m_fifo_head] >> (7 - m_fifo_bits_taken)) & 1);
    m_fifo_bits_taken++;
    if (m_fifo_bits_taken >= 8)
    {
      m_fifo_bits_taken = 0;
      m_fifo[m_fifo_head] = 0;
      m_fifo_head = (m_fifo_head + 1) % INPUT_FIFO_BYTES;
      m_fifo_count--;
      m_buffer_empty = (m_fifo_count == 0);
    }
  }
  return val;
}

// Parse one frame's worth of bits from the FIFO into m_new_frame_*. The
// frame format is the canonical TMS5220 layout:
//   4 bits  energy index  (0 = silence frame, 15 = stop frame, both early-exit)
//   1 bit   repeat flag   (skipped on silence/stop)
//   6 bits  pitch index   (0 = unvoiced → only first 4 k's, then early-exit)
//   5 bits  k1
//   5 bits  k2
//   4 bits  k3            (and 4 each for k4..k7)
//   3 bits  k8..k10
void Synth::parseFrame()
{
  m_uv_zpar = m_zpar = false;
#if TMS5220_DEBUG
  // Snapshot the FIFO byte offset and bit position at the start of this
  // parse so the log line can show "where in the stream we are."
  uint16_t dbg_start_head = m_fifo_head;
  uint8_t  dbg_start_bits = m_fifo_bits_taken;
  const char* dbg_exit_reason = "full";
#endif

  // Energy
  m_new_frame_energy_idx = (uint8_t)readBits(ENERGY_BITS);
  if (m_new_frame_energy_idx == 0 || m_new_frame_energy_idx == 0x0F)
  {
    // Silence (0) or stop (15) — process() handles these via the
    // newFrameStop()/newFrameSilence() predicates. No more bits to read.
#if TMS5220_DEBUG
    Serial.printf("[5220] @h=%u/b=%u  E=%u  %s\n",
                  dbg_start_head, dbg_start_bits,
                  m_new_frame_energy_idx,
                  m_new_frame_energy_idx == 0 ? "silence" : "STOP");
#endif
    return;
  }

  // Repeat flag — when set, reuse the previous frame's pitch and k's,
  // but still apply the new energy.
  int rep = readBits(1);

  // Pitch
  m_new_frame_pitch_idx = (uint8_t)readBits(PITCH_BITS);
  m_uv_zpar = newFrameUnvoiced();

  if (rep)
  {
#if TMS5220_DEBUG
    Serial.printf("[5220] @h=%u/b=%u  E=%u  R=1  P=%u  (repeat-reuse-K)\n",
                  dbg_start_head, dbg_start_bits,
                  m_new_frame_energy_idx,
                  m_new_frame_pitch_idx);
#endif
    return;   // keep prior k coefficients
  }

  for (int i = 0; i < 4; i++)
  {
    m_new_frame_k_idx[i] = (uint8_t)readBits(KBITS[i]);
  }

  // Unvoiced frames stop here — k5..k10 are forced to zero in the generator.
  if (m_new_frame_pitch_idx == 0)
  {
#if TMS5220_DEBUG
    dbg_exit_reason = "unvoiced-k1-4";
    Serial.printf("[5220] @h=%u/b=%u  E=%u  R=0  P=0  K=[%u,%u,%u,%u]  %s\n",
                  dbg_start_head, dbg_start_bits,
                  m_new_frame_energy_idx,
                  m_new_frame_k_idx[0], m_new_frame_k_idx[1],
                  m_new_frame_k_idx[2], m_new_frame_k_idx[3],
                  dbg_exit_reason);
#endif
    return;
  }

  for (int i = 4; i < NUM_K; i++)
  {
    m_new_frame_k_idx[i] = (uint8_t)readBits(KBITS[i]);
  }
#if TMS5220_DEBUG
  Serial.printf("[5220] @h=%u/b=%u  E=%u  R=0  P=%u  K=[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]\n",
                dbg_start_head, dbg_start_bits,
                m_new_frame_energy_idx, m_new_frame_pitch_idx,
                m_new_frame_k_idx[0], m_new_frame_k_idx[1],
                m_new_frame_k_idx[2], m_new_frame_k_idx[3],
                m_new_frame_k_idx[4], m_new_frame_k_idx[5],
                m_new_frame_k_idx[6], m_new_frame_k_idx[7],
                m_new_frame_k_idx[8], m_new_frame_k_idx[9]);
#endif
}

// Clip the 14-bit lattice output to the analog SPK pin's 10-bit DAC and
// then upshift back to 16 bits. Faithful to MAME's clip_analog().
//
// Real TMS5220 had two output options: this analog SPK pin (8 bits of
// real dynamic range after the funny clipping logic), or the digital I/O
// pin (full 12 bits). TI-99/4A cartridges used the analog pin; some
// arcade boards used digital. The analog clipping is part of the
// characteristic TMS5220 sound, but it costs ~12 dB of headroom and on
// modern speakers leaves speech perceptually quiet next to CALL SOUND
// tones. See digitalOutput() below for the alternative.
int16_t Synth::clipAnalog(int16_t v) const
{
  if (v >  2047) v =  2047;
  if (v < -2048) v = -2048;
  v &= ~0xF;
  // upshift + range-extend: top 11 bits → high bits, then replicate the
  // upper magnitude bits into the LSBs so silence maps to silence and full
  // scale maps to full scale.
  return (int16_t)((v << 4) | ((v & 0x7F0) >> 3) | ((v & 0x400) >> 10));
}

// Take the 14-bit lattice output and emit the chip's "digital I/O pin"
// value: top 12 bits, upshifted to 16-bit with magnitude bits replicated
// into the low bits. Faithful to MAME's m_digital_select==1 path.
//   input:  ssss ssss ssss ssss ssnn nnnn nnnn 0000
//   output: ssss ssss ssss ssss snnn nnnn nnnN NNNN
//   Where N replicates the upper magnitude bits so 0 → 0 and full → full.
//
// vs clipAnalog: ~+12 dB louder on the speaker, no analog-pin clipping.
// Historically inauthentic for the TI-99/4A (the cartridge used analog),
// but the Box-3 has no volume knob and we need the headroom.
int16_t Synth::digitalOutput(int16_t v) const
{
  int32_t s = (int32_t)v & ~0xF;
  return (int16_t)((s << 1) | ((s & 0x3E00) >> 9));
}

int32_t Synth::matrixMultiply(int32_t a, int32_t b) const
{
  // Wrap a into 10-bit signed (k coefficient range, -512..511) and b
  // into 14-bit signed (lattice accumulator, -16384..16383). MUST use
  // while-loops, not single-if conditionals: certain k×x products in
  // the lattice push values multiple wraps out of range, and a single
  // wrap leaves them outside the valid window — producing word-specific
  // garbled audio (the B-word block has K coefficient patterns that
  // hit this; many other words happen to stay in range).
  while (a >   511) a -= 1024;
  while (a <  -512) a += 1024;
  while (b >  16383) b -= 32768;
  while (b < -16384) b += 32768;
  return (a * b) >> 9;
}

// Unrolled 10-stage lattice filter (TMS5220 patent figure / MAME copy).
// m_u[10..0] = "top of lattice" (forward path), m_x[9..0] = "bottom of
// lattice" (backward path / delay line). Returns the 14-bit output.
int32_t Synth::latticeFilter()
{
  m_u[10] = matrixMultiply(m_previous_energy, (int32_t)m_excitation_data << 6);
  m_u[9]  = m_u[10] - matrixMultiply(m_current_k[9], m_x[9]);
  m_u[8]  = m_u[9]  - matrixMultiply(m_current_k[8], m_x[8]);
  m_u[7]  = m_u[8]  - matrixMultiply(m_current_k[7], m_x[7]);
  m_u[6]  = m_u[7]  - matrixMultiply(m_current_k[6], m_x[6]);
  m_u[5]  = m_u[6]  - matrixMultiply(m_current_k[5], m_x[5]);
  m_u[4]  = m_u[5]  - matrixMultiply(m_current_k[4], m_x[4]);
  m_u[3]  = m_u[4]  - matrixMultiply(m_current_k[3], m_x[3]);
  m_u[2]  = m_u[3]  - matrixMultiply(m_current_k[2], m_x[2]);
  m_u[1]  = m_u[2]  - matrixMultiply(m_current_k[1], m_x[1]);
  m_u[0]  = m_u[1]  - matrixMultiply(m_current_k[0], m_x[0]);

  m_x[9]  = m_x[8] + matrixMultiply(m_current_k[8], m_u[8]);
  m_x[8]  = m_x[7] + matrixMultiply(m_current_k[7], m_u[7]);
  m_x[7]  = m_x[6] + matrixMultiply(m_current_k[6], m_u[6]);
  m_x[6]  = m_x[5] + matrixMultiply(m_current_k[5], m_u[5]);
  m_x[5]  = m_x[4] + matrixMultiply(m_current_k[4], m_u[4]);
  m_x[4]  = m_x[3] + matrixMultiply(m_current_k[3], m_u[3]);
  m_x[3]  = m_x[2] + matrixMultiply(m_current_k[2], m_u[2]);
  m_x[2]  = m_x[1] + matrixMultiply(m_current_k[1], m_u[1]);
  m_x[1]  = m_x[0] + matrixMultiply(m_current_k[0], m_u[0]);
  m_x[0]  = m_u[0];

  m_previous_energy = (uint16_t)m_current_energy;
  return m_u[0];
}

int16_t Synth::getSample()
{
  if (!m_TALKD && !m_SPEN)
  {
    // Idle. Real chip outputs -1 on the analog pin between utterances;
    // we just emit silence.
    return 0;
  }

  // Frame boundary: IP=0, PC=12, subcycle=1 means "new frame fires now".
  if (m_IP == 0 && m_PC == 12 && m_subcycle == 1)
  {
    m_IP = RELOAD_TABLE[0];   // non-C 5220 → 0

    parseFrame();

    if (newFrameStop())
    {
      m_TALK = m_SPEN = false;
      // m_TALKD stays true while the energy ramps to zero over the next
      // interpolation block — same shutdown ramp the real chip does.

      // STOP frame terminates the current utterance. Drain the entire
      // FIFO: any bytes still queued belong to this phrase's trailing
      // padding (TI's encoder pads phrases to byte boundaries and may
      // leave whole bytes of 0x00 after the STOP code). Those zero
      // padding bytes would otherwise parse as endless silence frames
      // since silence is energy_idx=0 and a buffer-empty readBits also
      // returns zeros — the synth would never reach STOP again and
      // isTalking() would stay true until the host's timeout fires.
      //
      // For back-to-back CALL SAY calls, the host pushes the next
      // phrase via a new writeBytes() invocation — writeBytes' implicit
      // SPEAK-EXTERNAL re-arms m_SPEN at that point, so dropping the
      // FIFO here doesn't lose anything.
      //
      // For multi-word CALL SAY within a single utterance: the host
      // is expected to push one phrase at a time and wait for the
      // synth to drain between pushes (tiSay's per-word wait loop).
      m_fifo_head = 0;
      m_fifo_tail = 0;
      m_fifo_count = 0;
      m_fifo_bits_taken = 0;
      m_buffer_empty = true;
    }

    // Interpolation inhibit on voiced<->unvoiced or silence<->unvoiced
    // transitions, matching the MAME logic comment.
    bool inhibit =
        (!oldFrameUnvoiced() &&  newFrameUnvoiced()) ||
        ( oldFrameUnvoiced() && !newFrameUnvoiced()) ||
        ( oldFrameSilence() && !newFrameSilence())   ||
        ( oldFrameUnvoiced() &&  newFrameSilence());
    m_inhibit = inhibit;
  }
  else
  {
    // Interpolation happens on subcycle 2 of each PC: shift the delta
    // between current and target by INTERP_COEFF[m_IP] and add. When
    // m_inhibit is set and we haven't reached the last interp period,
    // the delta is suppressed (target snaps in at IP=0 only).
    bool inhibit_state = (m_inhibit && (m_IP != 0));
    int  shift = interpAt(m_IP);

    if (m_subcycle == 2)
    {
      switch (m_PC)
      {
        case 0:
          if (m_IP == 0) m_pitch_zero = false;
          if (!inhibit_state)
          {
            int32_t target = (int32_t)energyAt(m_new_frame_energy_idx);
            int32_t delta  = (target - m_current_energy) >> shift;
            m_current_energy += (int16_t)delta;
          }
          if (m_zpar) m_current_energy = 0;
          break;
        case 1:
          if (!inhibit_state)
          {
            int32_t target = (int32_t)pitchAt(m_new_frame_pitch_idx);
            int32_t delta  = (target - m_current_pitch) >> shift;
            m_current_pitch += (int16_t)delta;
          }
          if (m_zpar) m_current_pitch = 0;
          break;
        case 2: case 3: case 4: case 5: case 6:
        case 7: case 8: case 9: case 10: case 11:
        {
          int idx = m_PC - 2;
          if (!inhibit_state)
          {
            int32_t target = (int32_t)kAt(idx, m_new_frame_k_idx[idx]);
            int32_t delta  = (target - m_current_k[idx]) >> shift;
            m_current_k[idx] += (int16_t)delta;
          }
          bool zero_it = (idx < 4) ? m_zpar : (m_zpar || m_uv_zpar);
          if (zero_it) m_current_k[idx] = 0;
          break;
        }
        case 12: default: break;
      }
    }
  }

  // Generate excitation: white-ish noise for unvoiced (±0x40), or chirp
  // ROM addressed by pitch counter for voiced.
  if (oldFrameUnvoiced())
  {
    m_excitation_data = (m_RNG & 1) ? (int16_t)((int8_t)0xC0)
                                    : (int16_t)0x40;
  }
  else
  {
    m_excitation_data = (int16_t)chirpAt((uint8_t)m_pitch_count);
  }

  // Advance the RNG 20× per sample (one per T-cycle), same primitive as MAME.
  for (int i = 0; i < 20; i++)
  {
    int bitout = ((m_RNG >> 12) & 1) ^ ((m_RNG >> 3) & 1) ^
                 ((m_RNG >>  2) & 1) ^ ((m_RNG >> 0) & 1);
    m_RNG = (m_RNG << 1) | bitout;
  }

  int32_t s = latticeFilter();

  // Wrap to 14-bit signed (lattice output may have overflowed at k1 stage).
  while (s >  16383) s -= 32768;
  while (s < -16384) s += 32768;

  // Use the chip's digital-output pin formula (full 12-bit dynamic range)
  // rather than the analog SPK pin's clipping path. ~+12 dB louder; we
  // need the headroom on the Box-3's small speaker. To revert for
  // historical-accuracy purposes, swap to clipAnalog((int16_t)s).
  int16_t out = digitalOutput((int16_t)s);

  // Advance subcycle / PC / IP counters.
  m_subcycle++;
  if (m_subcycle == 2 && m_PC == 12)
  {
    if (m_IP == 7)
    {
      // RESETL4 — latch OLDE/OLDP, propagate TALK→TALKD, stop if needed.
      if (m_inhibit) m_pitch_zero = true;
      m_OLDE = newFrameSilence();
      m_OLDP = newFrameUnvoiced();
      m_TALKD = m_TALK;
      if (!m_TALK && m_SPEN) m_TALK = true;
    }
    m_subcycle = 1;
    m_PC = 0;
    m_IP = (m_IP + 1) & 0x7;
  }
  else if (m_subcycle == 3)
  {
    m_subcycle = 1;
    m_PC++;
  }

  // Pitch counter — wraps when it hits the current pitch period. The
  // pitch_zero flag (set when inhibit fires on a frame boundary) keeps
  // the counter pinned to 0 for ~2 samples.
  m_pitch_count++;
  if (m_pitch_count >= (uint16_t)m_current_pitch || m_pitch_zero)
  {
    m_pitch_count = 0;
  }
  m_pitch_count &= 0x1FF;

  return out;
}

} // namespace tms5220
