// audio.h
// Audio subsystem for the ESP32-S3-Box-3: ES8311 codec init + I²S DMA +
// SN76489-faithful tone/noise generator + TMS5220 speech synth. CALL
// SOUND routes via the tiSoundPlay/tiSoundStop overrides; CALL SAY
// routes via feedSpeechBytes() below.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Bring up the I²S bus, configure the ES8311 codec, enable the power
// amp on GPIO46, and start the audio mixing task. Idempotent.
void initAudio();

// TMS5220 speech: append LPC bytes to the synth's input FIFO. The mixer
// task pulls samples on its own. Returns the number of bytes accepted
// (drops the tail on overflow — 4 KB FIFO holds ~25 typical words).
//   Safe to call from any task; uses the audio critical section.
size_t feedSpeechBytes(const uint8_t* bytes, size_t count);

// True if the speech synth is currently producing audio (or has bytes
// queued that haven't started yet). Use from BASIC's CALL SAY wait loop.
bool isSpeechTalking();

// Stop speech immediately, discarding any queued bytes.
void stopSpeech();
