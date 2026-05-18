// audio.cpp
//
// ESP32-S3-Box-3 audio subsystem: ES8311 codec init via I²C + I²S DMA
// output via the chip's I²S0 peripheral + SN76489-faithful tone & noise
// generator running in a FreeRTOS task. Strong overrides of the weak
// tiSoundPlay / tiSoundStop symbols declared in the shared interpreter's
// ti_platform.h, so CALL SOUND in BASIC routes straight here.
//
// SN76489 (TMS9919) emulation:
//   - 3 tone voices, square waves, frequency programmable per voice
//   - 1 noise voice with 16-bit LFSR (taps at bits 0,3 → output at bit 0)
//     - Periodic noise types -1..-3: clock divisions 256 / 512 / 1024
//     - Periodic noise type   -4: clocked by tone voice 3
//     - White noise types -5..-7: same divisions but LFSR feedback
//     - White noise type  -8: clocked by tone voice 3
//   - 16-step volume table at -2 dB increments (TI vol 0..30 → idx 0..15)
//
// Sample rate: 22050 Hz mono is plenty for SN76489 (max audible ~3.5 kHz
// from a real chip). Cuts I²S bandwidth and CPU load roughly in half vs
// 44100 Hz.

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <driver/i2s_std.h>
#include "audio.h"
#include "ti_platform.h"
#include "tms5220.h"

// ---------------------------------------------------------------------------
// Box-3 BSP pin constants (per esp-bsp/esp-box-3 + sheet 3 of MB schematic).
// ---------------------------------------------------------------------------
#define I2S_MCLK_PIN  GPIO_NUM_2
#define I2S_BCLK_PIN  GPIO_NUM_17
#define I2S_LRCK_PIN  GPIO_NUM_45
#define I2S_DOUT_PIN  GPIO_NUM_15
#define I2C_SDA_PIN   GPIO_NUM_8
#define I2C_SCL_PIN   GPIO_NUM_18
#define PA_CTRL_PIN   GPIO_NUM_46
#define ES8311_ADDR   0x18

#define SAMPLE_RATE   44100
#define DMA_FRAMES    256

// 1-pole IIR low-pass at ~5 kHz cutoff. Real SN76489-output cabinets had
// passive RC filtering at the chip output that softened the sharp square
// edges into something that sounded like a tone instead of a buzz. We
// approximate that here in software — keeps the chiptune character but
// kills the worst of the high-frequency rasp.
//   α = 2π·fc / (SR + 2π·fc) for fc = 5000, SR = 44100  ≈  0.416
static const float LPF_ALPHA = 0.416f;

// PolyBLEP residual at a unit step discontinuity. Used to antialias the
// sharp transitions of the tone voices' square waves; without it,
// harmonics of high-frequency tones fold back into the audio band as
// faint inharmonic squeals at our 44.1 kHz sample rate. Reference:
// Välimäki, "Antialiasing Oscillators in Subtractive Synthesis."
//
//   t  in [0..1)  : normalized phase
//   dt in (0..1)  : per-sample normalized step
static inline float poly_blep(float t, float dt)
{
  if (t < dt)
  {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt)
  {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

// Bandlimited square wave at the given phase / step. Output is in [-1, +1]
// before any volume scaling. PolyBLEP smooths the two discontinuities per
// cycle (at phase=0 falling edge and phase=0.5 rising edge).
static inline float bandlimited_square(uint32_t phase, uint32_t step)
{
  // Convert 32-bit fixed-point phase / step to normalized [0..1).
  const float INV_2POW32 = 1.0f / 4294967296.0f;
  float t  = (float)phase * INV_2POW32;
  float dt = (float)step  * INV_2POW32;

  float naive = (phase & 0x80000000u) ? +1.0f : -1.0f;
  float corr_down = poly_blep(t, dt);                   // falling edge at t=0
  float t2 = t + 0.5f;
  if (t2 >= 1.0f) t2 -= 1.0f;
  float corr_up   = poly_blep(t2, dt);                  // rising edge at t=0.5
  return naive - corr_down + corr_up;
}

// ---------------------------------------------------------------------------
// SN76489 16-step volume table (16-bit signed, peak ±32767).
// 0 dB, then -2 dB per step until silence.
// ---------------------------------------------------------------------------
static const int16_t SN_VOL[16] = {
  32767, 26031, 20674, 16422, 13044, 10362,  8231,  6536,
   5193,  4124,  3275,  2602,  2067,  1641,  1304,     0
};

// ---------------------------------------------------------------------------
// Per-voice state. Phase is 32.0 fixed point; step adds per-sample.
// Square wave is +amp when phase MSB = 1, -amp when MSB = 0.
// ---------------------------------------------------------------------------
struct Voice
{
  uint32_t phase     = 0;
  uint32_t step      = 0;     // phase increment per sample
  int      vol_idx   = 15;    // SN volume table index (15 = silent)
};
static Voice g_tone[3];        // voices 1..3
static Voice g_noise;          // voice 4 (LFSR)

// TMS5220 speech voice — fed from the BASIC CALL SAY / SPGET path, pulled
// at 8 kHz from inside the mixer at 44100 Hz with a linear-interpolating
// fractional-phase resampler. The synth state is guarded by g_audio_mux
// so that feedSpeechBytes() from a non-audio task is safe.
//
// Step value: 8000 / 44100 ≈ 0.18141; encoded as 24.8 fixed-point
// (256 * 8000 / 44100 = 46). The whole-number part is "how many native
// 8 kHz samples to advance this mixer tick", and the fraction is the
// crossfade between the previous and current native samples.
static tms5220::Synth g_speech;
static const uint32_t SPEECH_STEP_Q8 =
  (uint32_t)((256ULL * (uint64_t)tms5220::SAMPLE_RATE_HZ) /
             (uint64_t)SAMPLE_RATE);
static uint32_t g_speech_phase_q8 = 0;   // fractional phase 0..255
static int16_t  g_speech_prev = 0;       // previous native 8 kHz sample
static int16_t  g_speech_curr = 0;       // current  native 8 kHz sample

// Volume state (CALL VOLUME / CALL SPVOL). Both use SN76489's 0..30 scale
// internally (0 = loudest, 30 = silent) to match CALL SOUND's vol arg.
//
//   g_vol_master: maps linearly to the ES8311 reg 0x32 attenuator at
//                 -1.5 dB per step. n=0 → 0xBF (0 dB), n=30 → 0x83 (-30 dB).
//   g_speech_mix: maps to a float mixer multiplier. n=0 → 5.0× (loud),
//                 n=15 → 2.5× (mid), n=30 → 0.0× (silent). Higher than
//                 the 2.0× we started with: empirically the TMS5220's
//                 lattice output is small enough that even ±32767-peak
//                 samples sit deep in the tanh limiter's linear region,
//                 so they never get the perceptual loudness boost that
//                 CALL SOUND's full-scale squares get from being mildly
//                 compressed. Pushing speech harder lets it reach the
//                 tanh knee, which closes most of the gap.
//
// Loaded from NVS at initAudio(); CALL VOLUME / CALL SPVOL update both
// the live values and the NVS-persisted store. Defaults if no NVS entry:
// master=0 (loudest, matches the codec setting we hard-coded for boot),
// speech=0 (loudest, matches the 2.0× mixer we shipped in d2edc7e).
static volatile int   g_vol_master  = 0;
static volatile float g_speech_mix  = 5.0f;

// Forward declaration so initAudio() can call this; definition lives
// alongside the other volume helpers at the end of the file.
static void loadVolumeFromNvs();

// LFSR state for noise voice. SN76489: 16-bit, taps at 0 and 3 (→ output bit 0).
// Initial seed is the standard "single bit" so the first transition is clean.
static uint16_t g_lfsr      = 0x8000;
static bool     g_noise_white = false;   // true = white (LFSR), false = periodic
static int      g_noise_clock_voice = -1; // -1 = use g_noise.step; else 0..2 → tone voice idx

// Sound auto-stop timer. Set by tiSoundPlay; checked by audio task.
static volatile unsigned long g_sound_end_at = 0;
static portMUX_TYPE g_audio_mux = portMUX_INITIALIZER_UNLOCKED;

// I²S TX channel handle.
static i2s_chan_handle_t g_tx_chan = NULL;
static bool              g_audio_ready = false;

// ---------------------------------------------------------------------------
// Tiny I²C helper for the codec — uses the global Wire bus shared with the
// touch controller.
// ---------------------------------------------------------------------------
static bool es8311_w(uint8_t reg, uint8_t val)
{
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// ---------------------------------------------------------------------------
// ES8311 codec init for 22050 Hz mono I²S DAC output.
//
// Register sequence is the well-documented Espressif boot path: reset,
// configure clock manager from MCLK pin (we feed MCLK at 256 × Fs from the
// I²S peripheral), set 16-bit Philips format, power up DAC + HP output,
// bypass the equalizer, set a sane DAC volume.
//
// Returns true if the codec ACK'd every transaction.
// ---------------------------------------------------------------------------
static bool es8311_init()
{
  // Reset
  if (!es8311_w(0x00, 0x1F)) return false;
  delay(20);
  es8311_w(0x00, 0x00);
  es8311_w(0x00, 0x80);   // power-on (slave mode)

  // Clock manager values per Espressif's es8311 driver coeff_div lookup
  // for MCLK = 256 * Fs (works for any sample rate in that mode).
  //   Reg01 = 0x3F: enable all internal clocks + MCLK source = MCLK pin.
  //                 (Earlier 0x30 missed the lower clock-enable bits.)
  //   Reg06 = 0x04: BCLK divider for 64*Fs frame in 256x MCLK mode.
  es8311_w(0x01, 0x3F);
  es8311_w(0x02, 0x00);
  es8311_w(0x03, 0x10);   // ADC OSR
  es8311_w(0x04, 0x10);   // DAC OSR
  es8311_w(0x05, 0x00);
  es8311_w(0x06, 0x04);
  es8311_w(0x07, 0x00);   // LRCK divider H
  es8311_w(0x08, 0xFF);   // LRCK divider L

  // Serial data port: 16-bit Philips on both directions.
  es8311_w(0x09, 0x0C);   // SDP IN  (we don't use it, but keep matched)
  es8311_w(0x0A, 0x0C);   // SDP OUT 16-bit

  // Power-up sequence
  es8311_w(0x0B, 0x00);
  es8311_w(0x0C, 0x00);
  es8311_w(0x10, 0x1F);   // power management — analog blocks
  es8311_w(0x11, 0x7F);
  es8311_w(0x00, 0x80);   // CSM power on (after analog ready)
  es8311_w(0x0D, 0x01);
  es8311_w(0x0E, 0x02);

  // DAC enable, HP output enable
  es8311_w(0x12, 0x00);
  es8311_w(0x13, 0x10);
  es8311_w(0x14, 0x1A);   // DAC mute disable

  // Bypass equalizers (we want clean square waves)
  es8311_w(0x1C, 0x6A);
  es8311_w(0x37, 0x08);

  // DAC volume — ES8311 register 0x32 is a linear attenuator: 0xBF = 0 dB
  // (max), each step down = -0.5 dB. We run at 0xBC ≈ -1.5 dB, leaving a
  // sliver of headroom below 0 dB. The original -16 dB setting was tuned
  // conservatively for SN76489 square edges before speech existed; once we
  // added the TMS5220 voice (lower RMS-to-peak ratio than a square wave)
  // it sat well below speaker-level. The tanh soft-limit + LPF keep tones
  // from getting harsh at this level. Will be runtime-settable via
  // CALL VOLUME() once that BASIC extension lands.
  es8311_w(0x32, 0xBC);

  return true;
}

// ---------------------------------------------------------------------------
// I²S TX init at 22050 Hz, 16-bit, mono. MCLK on its own pin so the codec
// can use it as its clock reference (matches our es8311 reg-1 setting).
// ---------------------------------------------------------------------------
static bool i2s_init_tx()
{
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                          I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan_cfg, &g_tx_chan, NULL) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_MCLK_PIN,
      .bclk = I2S_BCLK_PIN,
      .ws   = I2S_LRCK_PIN,
      .dout = I2S_DOUT_PIN,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { 0, 0, 0 },
    },
  };
  // 256× MCLK matches our codec config above.
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  if (i2s_channel_init_std_mode(g_tx_chan, &std_cfg) != ESP_OK) return false;
  if (i2s_channel_enable(g_tx_chan) != ESP_OK) return false;
  return true;
}

// ---------------------------------------------------------------------------
// One LFSR step.
//   White-noise mode: 16-bit Galois LFSR with primitive-polynomial mask
//   0xB400 (taps at 0, 2, 3, 5). Maximal period 65535, no dead state as
//   long as the seed is non-zero. The earlier hand-rolled XOR-feedback
//   variant locked into the all-zeros absorbing state after ~16 shifts.
//   Periodic mode: rotate bit 0 back to bit 15 — output is a steady
//   square pulse at the clock-divider rate (the classic TI "drum-ish"
//   periodic noise).
// Output is +1 / -1 from the bit that gets shifted out, suitable for
// summing directly into the mixer.
// ---------------------------------------------------------------------------
static inline int8_t lfsr_step()
{
  uint16_t out = g_lfsr & 1;
  if (g_noise_white)
  {
    g_lfsr >>= 1;
    if (out) g_lfsr ^= 0xB400;
  }
  else
  {
    g_lfsr = (g_lfsr >> 1) | (out << 15);
  }
  return out ? +1 : -1;
}

// ---------------------------------------------------------------------------
// Audio mixing task. Pinned to core 1 so it doesn't compete with the BLE
// stack (core 0). Fills DMA buffers continuously; samples not in active
// CALL SOUND windows are zeros (silence — keeps DMA flowing and avoids
// pop-on-resume).
// ---------------------------------------------------------------------------
static void audioTask(void* /*arg*/)
{
  // Stereo I²S frame is 2 × int16_t even if we mix mono.
  int16_t buf[DMA_FRAMES * 2];
  float   lpf_y = 0.0f;     // 1-pole IIR low-pass state

  while (true)
  {
    // Auto-silence if past the scheduled end time.
    if (millis() >= g_sound_end_at)
    {
      portENTER_CRITICAL(&g_audio_mux);
      g_tone[0].vol_idx = g_tone[1].vol_idx = g_tone[2].vol_idx = 15;
      g_noise.vol_idx = 15;
      portEXIT_CRITICAL(&g_audio_mux);
    }

    for (int i = 0; i < DMA_FRAMES; i++)
    {
      float mix = 0.0f;

      // Tone voices — bandlimited squares via PolyBLEP. Without this,
      // harmonics above Nyquist fold back into the audio band as faint
      // inharmonic squeals on high-pitch tones.
      for (int v = 0; v < 3; v++)
      {
        Voice& vc = g_tone[v];
        if (vc.vol_idx >= 15 || vc.step == 0) continue;
        vc.phase += vc.step;
        float amp = (float)SN_VOL[vc.vol_idx];
        mix += bandlimited_square(vc.phase, vc.step) * amp;
      }

      // Noise voice — clocked either by its own step OR by tone voice 3.
      // No bandlimiting: white noise is broadband by design and periodic
      // noise is supposed to sound buzzy.
      if (g_noise.vol_idx < 15)
      {
        bool clock_tick = false;
        if (g_noise_clock_voice == 2)
        {
          static uint32_t last_phase_msb = 0;
          uint32_t cur = g_tone[2].phase & 0x80000000u;
          if (cur != last_phase_msb)
          {
            clock_tick = true;
            last_phase_msb = cur;
          }
        }
        else if (g_noise.step != 0)
        {
          uint32_t prev = g_noise.phase;
          g_noise.phase += g_noise.step;
          if ((g_noise.phase & 0x80000000u) != (prev & 0x80000000u))
          {
            clock_tick = true;
          }
        }
        static int8_t noise_out = +1;
        if (clock_tick) noise_out = lfsr_step();
        mix += (float)(noise_out * SN_VOL[g_noise.vol_idx]);
      }

      // Headroom: scale by 1/3 so 3 simultaneous voices at full SN
      // volume sit below the soft-limit's knee. tanh handles whatever
      // does exceed full scale gracefully.
      mix *= (1.0f / 3.0f);

      // 1-pole low-pass — finishes the job PolyBLEP started on tone
      // voices, and smooths the noise voice's hard clock edges.
      lpf_y += LPF_ALPHA * (mix - lpf_y);

      // TMS5220 speech voice — runs at 8 kHz native, fractional-phase
      // resample to our 44.1 kHz mix rate. Linear interpolation
      // between the previous and current native sample. We pull a new
      // native sample every time the fractional phase carries past 256.
      // Routed AFTER the LPF (which is tuned for SN76489 square edges)
      // because the speech-band content above 3 kHz carries fricative
      // intelligibility — running it through a 5 kHz LPF muffles "s" /
      // "sh" / "f" badly.
      g_speech_phase_q8 += SPEECH_STEP_Q8;
      while (g_speech_phase_q8 >= 256)
      {
        g_speech_phase_q8 -= 256;
        g_speech_prev = g_speech_curr;
        g_speech_curr = g_speech.getSample();
      }
      float speech_sample =
        (float)g_speech_prev +
        ((float)(g_speech_curr - g_speech_prev) *
         (float)g_speech_phase_q8 * (1.0f / 256.0f));

      // Mix speech in at the runtime-settable speech volume (default 2.0×,
      // ~+6 dB above unity; the TMS5220's lattice output is mostly in
      // ±8000 even on vowel peaks). CALL SPVOL() adjusts g_speech_mix
      // between 0.0× (silent) and 2.0× (loudest). The tanh soft-limit
      // below absorbs brief excitation-chirp peaks that overshoot full
      // scale at higher settings.
      float final_mix = lpf_y + speech_sample * g_speech_mix;

      // tanh soft-limit — replaces hard int16 clipping. Real SN76489's
      // analog output stage compressed overshoots gracefully; tanh
      // mimics that, so multi-voice peaks roll off into compression
      // instead of squaring into harsh distortion. With speech added,
      // tanh also keeps the loud chirp peaks from clipping mid-vowel.
      float normalized = final_mix * (1.0f / 32767.0f);
      int16_t s = (int16_t)(tanhf(normalized) * 32767.0f);

      buf[i * 2 + 0] = s;
      buf[i * 2 + 1] = s;
    }

    size_t bytes_written = 0;
    i2s_channel_write(g_tx_chan, buf, sizeof(buf), &bytes_written,
                      portMAX_DELAY);
  }
}

// ---------------------------------------------------------------------------
// Public init. Safe to call once from setup() after Wire.begin() has
// been done by another subsystem (touch). If Wire isn't initialised yet,
// initialises it here.
// ---------------------------------------------------------------------------
void initAudio()
{
  if (g_audio_ready) return;

  // Make sure I²C is up. LovyanGFX's touch driver may have already done
  // this with the same pins; calling Wire.begin() twice is harmless.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);

  if (!es8311_init())
  {
    Serial.println("[audio] ES8311 init failed (no I2C ACK at 0x18)");
    return;
  }
  Serial.println("[audio] ES8311 codec OK");

  if (!i2s_init_tx())
  {
    Serial.println("[audio] I2S init failed");
    return;
  }
  Serial.println("[audio] I2S TX up at 22050 Hz mono");

  // Enable power amplifier — drives the speaker.
  pinMode(PA_CTRL_PIN, OUTPUT);
  digitalWrite(PA_CTRL_PIN, HIGH);

  // Spawn the mixing task on core 1 (BLE / Wi-Fi own core 0).
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL,
                          configMAX_PRIORITIES - 4, NULL, 1);

  g_audio_ready = true;
  Serial.println("[audio] PA on, audio task running");

  // Load persisted volume settings from NVS. Has to come AFTER g_audio_ready
  // since the load applies values through helpers that check that flag.
  // Boot beep below runs at whatever level NVS specifies — so users who
  // turned the volume way down get a quiet boot beep on next power-up,
  // matching expectations.
  loadVolumeFromNvs();

  // Boot beep: 200 ms 800 Hz tone, full volume. If you hear this, the
  // entire audio chain works end-to-end and any silence on CALL SOUND
  // is in the BASIC dispatch, not the codec/DMA/PA. If you don't hear
  // it, codec config is producing silence.
  // TI-99/4A authentic boot beep: 1396.91 Hz (≈ F6) for 166 ms.
  portENTER_CRITICAL(&g_audio_mux);
  g_tone[0].phase   = 0;
  // step = freq * 2^32 / SR  (see step_from_freq below for the same math).
  g_tone[0].step    = (uint32_t)(((uint64_t)1397ULL << 32) / (uint64_t)SAMPLE_RATE);
  g_tone[0].vol_idx = 0;            // full SN volume
  g_sound_end_at    = millis() + 166;
  portEXIT_CRITICAL(&g_audio_mux);
  Serial.println("[audio] boot beep: 1397 Hz / 166 ms (TI-authentic)");
}

// ---------------------------------------------------------------------------
// Strong overrides of the interpreter's weak tiSoundPlay / tiSoundStop.
// ---------------------------------------------------------------------------

static inline int sn_index_from_ti_vol(int v)
{
  // TI CALL SOUND volume: 0 = loudest, 30 = silent.
  // SN76489 attenuator:    0 = loudest, 15 = silent.
  // Halve, clamp.
  int idx = v / 2;
  if (idx < 0)  idx = 0;
  if (idx > 15) idx = 15;
  return idx;
}

static inline uint32_t step_from_freq(int freq)
{
  // step = freq * 2^32 / SAMPLE_RATE (so MSB toggles 2*freq times per sec
  // for the square-wave path). Use 64-bit math for the divide.
  return (uint32_t)(((uint64_t)(uint32_t)freq << 32) / (uint64_t)SAMPLE_RATE);
}

void tiSoundPlay(int duration,
                 int f1, int v1, int f2, int v2,
                 int f3, int v3, int f4, int v4)
{
  if (!g_audio_ready) return;

  int freqs[4] = {f1, f2, f3, f4};
  int vols[4]  = {v1, v2, v3, v4};

  portENTER_CRITICAL(&g_audio_mux);

  // Reset all voices; we'll fill them by routing argument pairs below.
  for (int i = 0; i < 3; i++)
  {
    g_tone[i].step    = 0;
    g_tone[i].vol_idx = 15;
  }
  g_noise.vol_idx     = 15;
  g_noise.step        = 0;
  g_noise_clock_voice = -1;

  // Route by sign — not by argument position. On real TI Extended BASIC
  // the noise voice can be the 1st, 2nd, 3rd or 4th (freq, vol) pair;
  // negative freq (-1..-8) marks it as noise, positive freq is a tone.
  // E.g. CALL SOUND(-50,-5,V) is a single noise-voice call (no tones).
  int tone_idx = 0;
  for (int i = 0; i < 4; i++)
  {
    int f = freqs[i];
    if (f == 0) continue;                         // unused / silent slot

    if (f < 0 && f >= -8)
    {
      // Noise voice — type selects clock division and LFSR mode.
      g_noise.vol_idx = sn_index_from_ti_vol(vols[i]);
      // White noise: -5..-8.  Periodic noise: -1..-4.
      g_noise_white = (f >= -8 && f <= -5);
      if (f == -4 || f == -8)
      {
        // Clock the noise from tone voice 3.
        g_noise_clock_voice = 2;
        g_noise.step        = 0;
      }
      else
      {
        // Periodic divisions: -1/-5 → 256, -2/-6 → 512, -3/-7 → 1024.
        int abs_t  = -f;
        int divisor = 256;
        if (abs_t == 2 || abs_t == 6) divisor = 512;
        if (abs_t == 3 || abs_t == 7) divisor = 1024;
        // SN76489 master clock ≈ 3.579545 MHz, then /16 internal pre-div.
        double freq = 3579545.0 / (16.0 * divisor);
        g_noise.step = step_from_freq((int)freq);
      }
    }
    else if (f >= 110 && f <= 40000 && tone_idx < 3)
    {
      // Tone voice — fill the next available tone slot in order.
      g_tone[tone_idx].step    = step_from_freq(f);
      g_tone[tone_idx].vol_idx = sn_index_from_ti_vol(vols[i]);
      tone_idx++;
    }
  }

  unsigned long absDur = (duration >= 0) ? duration : -duration;
  if (absDur > 4250) absDur = 4250;
  g_sound_end_at = millis() + absDur;

  portEXIT_CRITICAL(&g_audio_mux);
}

void tiSoundStop()
{
  if (!g_audio_ready) return;
  portENTER_CRITICAL(&g_audio_mux);
  g_tone[0].vol_idx = g_tone[1].vol_idx = g_tone[2].vol_idx = 15;
  g_noise.vol_idx = 15;
  g_sound_end_at = 0;
  portEXIT_CRITICAL(&g_audio_mux);
}

// ---------------------------------------------------------------------------
// Public speech-synth API. The synth runs in the audio task; these
// functions just push bytes / queries into it under the same critical
// section the SN76489 voices use. Safe to call from BASIC's main thread.
// ---------------------------------------------------------------------------

size_t feedSpeechBytes(const uint8_t* bytes, size_t count)
{
  if (!g_audio_ready || bytes == nullptr || count == 0) return 0;
  portENTER_CRITICAL(&g_audio_mux);
  size_t n = g_speech.writeBytes(bytes, count);
  portEXIT_CRITICAL(&g_audio_mux);
  return n;
}

bool isSpeechTalking()
{
  if (!g_audio_ready) return false;
  portENTER_CRITICAL(&g_audio_mux);
  bool t = g_speech.isTalking();
  portEXIT_CRITICAL(&g_audio_mux);
  return t;
}

void stopSpeech()
{
  if (!g_audio_ready) return;
  portENTER_CRITICAL(&g_audio_mux);
  g_speech.reset();
  g_speech_phase_q8 = 0;
  g_speech_prev = g_speech_curr = 0;
  portEXIT_CRITICAL(&g_audio_mux);
}

// ---------------------------------------------------------------------------
// CALL VOLUME / CALL SPVOL / CALL GETVOLUME / CALL GETSPVOL.
// All four use the SN76489 0..30 scale: 0 = loudest, 30 = silent.
// Strong overrides of the interpreter's weak tiSetVolume / etc.
// Persisted to NVS so the setting survives reboots — important on
// hardware without a physical volume knob.
// ---------------------------------------------------------------------------

// 0..30 → ES8311 reg 0x32 value. -1.5 dB per step (3 register steps =
// 1 input step, since each register step is 0.5 dB). Clamped to [0x83,0xBF]
// so n=30 sits at -30 dB rather than driving the register all the way to
// silent (0xBF - 30*3 = 0x83 = -30 dB, plenty for "minimum").
static uint8_t masterRegFromN(int n)
{
  if (n < 0)  n = 0;
  if (n > 30) n = 30;
  return (uint8_t)(0xBF - (n * 3));
}

void tiSetVolume(int n)
{
  if (!g_audio_ready) return;
  if (n < 0)  n = 0;
  if (n > 30) n = 30;
  g_vol_master = n;
  es8311_w(0x32, masterRegFromN(n));
  Preferences prefs;
  prefs.begin("boxbasic", false);
  prefs.putUChar("volMaster", (uint8_t)n);
  prefs.end();
}

void tiGetVolume(int* out)
{
  if (out) *out = g_vol_master;
}

void tiSetSpeechVolume(int n)
{
  if (n < 0)  n = 0;
  if (n > 30) n = 30;
  // Linear 0..30 → 2.0..0.0× mixer multiplier. Half-scale (n=15) lands
  // at the unity 1.0× point so users have a clear "default" feel.
  float gain = 5.0f * (1.0f - (float)n / 30.0f);
  portENTER_CRITICAL(&g_audio_mux);
  g_speech_mix = gain;
  portEXIT_CRITICAL(&g_audio_mux);
  Preferences prefs;
  prefs.begin("boxbasic", false);
  prefs.putUChar("volSpeech", (uint8_t)n);
  prefs.end();
}

void tiGetSpeechVolume(int* out)
{
  if (!out) return;
  // Reverse the linear map. Round to nearest integer for cleanliness.
  float g = g_speech_mix;
  int n = (int)(30.0f * (1.0f - g / 5.0f) + 0.5f);
  if (n < 0)  n = 0;
  if (n > 30) n = 30;
  *out = n;
}

// Called once from initAudio() to load NVS-persisted volume settings.
// Applies them via the same Set functions so codec/mixer state stays
// in sync with the live g_vol_* values. No-op if no NVS entry exists
// (defaults shipped in the globals stand).
static void loadVolumeFromNvs()
{
  Preferences prefs;
  prefs.begin("boxbasic", true);  // read-only
  bool hasMaster = prefs.isKey("volMaster");
  bool hasSpeech = prefs.isKey("volSpeech");
  uint8_t master = hasMaster ? prefs.getUChar("volMaster", 0) : 0;
  uint8_t speech = hasSpeech ? prefs.getUChar("volSpeech", 0) : 0;
  prefs.end();
  if (hasMaster)
  {
    g_vol_master = master;
    es8311_w(0x32, masterRegFromN(master));
  }
  if (hasSpeech)
  {
    g_speech_mix = 5.0f * (1.0f - (float)speech / 30.0f);
  }
}
