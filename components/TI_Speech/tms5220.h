// TI_Speech/tms5220.h
//
// Embeddable TMS5220 speech-synthesizer core, ported from MAME's
// src/devices/sound/tms5220.cpp (commit master). The MAME implementation is
// BSD-3-Clause; license terms below preserve the original copyright. This
// host-agnostic adaptation strips out:
//
//   - MAME's device_t / sound_stream_update machinery
//   - The TMS5220 command FIFO + bus protocol (RS/WS pins, IRQ pin, status
//     register, READY handshaking)
//   - VSM speech-ROM clocking (M0/M1/ADDR/DATA pin emulation)
//   - The TMS5220C variable-frame-rate feature
//   - LOGMASKED() debug spew
//
// What remains is the audio DSP: parse a stream of TMS5220 LPC bytes into
// 4-bit energy / 1-bit repeat / 6-bit pitch / 5-or-3-bit k-coefficient
// frames, interpolate between consecutive frames in 7 sub-steps (the chip's
// 'IP' counter), and run the 10-stage lattice filter at 8 kHz to produce
// 16-bit PCM samples.
//
// Frames are fed via writeBytes(); samples are pulled via getSample(). The
// caller owns rate conversion to the codec's sample rate (we run at the
// TMS5220's native 8 kHz). The class is a single-talker (one playback at a
// time) — same as the real chip.
//
// Original MAME source: SPDX-License-Identifier: BSD-3-Clause
// Original copyright holders: Frank Palazzolo, Aaron Giles, Jonathan
// Gevaryahu, Raphael Nabet, Couriersud, Michael Zapf
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//   1. Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//   2. Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//   3. Neither the name of the copyright holder nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Debug instrumentation. When defined non-zero, parseFrame() prints a
// single Serial.printf line per parsed frame showing energy / repeat /
// pitch / k1..k10 indices. Use to diagnose audible garble on specific
// words: flash with this enabled, run CALL SAY("WORD"), inspect the
// frame stream. Adds ~150 bytes per call and small CPU overhead, so
// keep at 0 for normal builds. The print routes through Arduino's
// Serial, which is initialized by the host's setup() — we don't bring
// it up here.
#ifndef TMS5220_DEBUG
#define TMS5220_DEBUG 0
#endif

namespace tms5220
{

// Native output sample rate of the TMS5220 (clock 640 kHz / 80).
static constexpr int SAMPLE_RATE_HZ = 8000;

// Maximum number of buffered LPC bytes waiting to be parsed into frames.
// A typical "HELLO" word is ~150 bytes; this size lets a few words queue
// up without the host having to feed a byte at a time.
static constexpr size_t INPUT_FIFO_BYTES = 4096;

class Synth
{
public:
  Synth();

  // Hard reset: clear FIFO, stop talking, zero filter state.
  void reset();

  // Append LPC bytes to the input FIFO. Returns the number actually
  // accepted (silently drops the tail on FIFO overflow). Calling this
  // after a STOP frame implicitly restarts speech on the next sample.
  size_t writeBytes(const uint8_t* bytes, size_t count);

  // Are we either currently producing samples, or have bytes queued
  // that haven't started speaking yet?
  bool isTalking() const { return m_SPEN || m_TALKD || m_fifo_count > 0; }

  // Generate the next 16-bit signed sample at 8 kHz. Returns 0 when
  // idle; returns the lattice output when speaking. Caller is responsible
  // for resampling to the codec's rate.
  int16_t getSample();

private:
  // --- bit-stream input ---------------------------------------------------
  uint8_t  m_fifo[INPUT_FIFO_BYTES];
  uint16_t m_fifo_head;          // next byte to read
  uint16_t m_fifo_tail;          // next slot to write
  uint16_t m_fifo_count;         // bytes in flight
  uint8_t  m_fifo_bits_taken;    // bits already consumed from m_fifo[head]
  bool     m_buffer_empty;       // true once FIFO is drained

  int  readBits(int count);

  // --- chip state ---------------------------------------------------------
  bool m_SPEN;       // speak-external in progress (we feed via FIFO)
  bool m_TALK;       // set on SPEN after first frame, cleared on stop
  bool m_TALKD;      // delayed TALK, latched at IP=7 PC=12 RESETL4

  // current decoded frame parameters
  uint8_t m_new_frame_energy_idx;
  uint8_t m_new_frame_pitch_idx;
  uint8_t m_new_frame_k_idx[10];

  // interpolated (current sample) values
  int16_t m_current_energy;
  int16_t m_current_pitch;
  int16_t m_current_k[10];
  uint16_t m_previous_energy;

  bool m_OLDE;       // old frame had energy == 0
  bool m_OLDP;       // old frame was unvoiced
  bool m_inhibit;    // interpolation inhibited this frame
  bool m_uv_zpar;    // zero k5..k10 (unvoiced frame)
  bool m_zpar;       // zero ALL parameters (between frames)
  bool m_pitch_zero; // pitch counter held at 0

  // chip cycle counters: PC = 0..12 (param counter), IP = 0..7 (interp
  // period), subcycle = 1 or 2 (A/B half).
  uint8_t  m_PC;
  uint8_t  m_IP;
  uint8_t  m_subcycle;
  uint16_t m_pitch_count;
  uint16_t m_RNG;
  int16_t  m_excitation_data;

  // 10-stage lattice filter state
  int32_t m_u[11];
  int32_t m_x[10];

  // --- private DSP --------------------------------------------------------
  void    parseFrame();
  int32_t latticeFilter();
  int32_t matrixMultiply(int32_t a, int32_t b) const;
  int16_t clipAnalog(int16_t v) const;     // SPK pin emulation (quieter, period-authentic)
  int16_t digitalOutput(int16_t v) const;  // digital pin emulation (~+12 dB louder, currently active)

  // Helpers for the frame-flag predicates the original used as members.
  bool oldFrameSilence()    const { return m_OLDE; }
  bool oldFrameUnvoiced()   const { return m_OLDP; }
  bool newFrameStop()       const { return m_new_frame_energy_idx == 0x0F; }
  bool newFrameSilence()    const { return m_new_frame_energy_idx == 0; }
  bool newFrameUnvoiced()   const { return m_new_frame_pitch_idx  == 0; }
};

} // namespace tms5220
