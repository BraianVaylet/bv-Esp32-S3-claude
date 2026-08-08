#pragma once
#include <Arduino.h>

// Spoken alerts through the onboard ES8311 codec and speaker.
//
// IMPORTANT: call audio_begin() *before* display_begin().
//
// The ES8311 driver talks over Arduino's `Wire`, which owns I2C port 0, and
// LovyanGFX wants that same port for the CST816 touch controller on the same
// pins. Rather than run two masters on one bus, the codec is configured once
// during boot and the bus is then released, so LovyanGFX gets it uncontested.
// Nothing here touches I2C again afterwards: playback is I2S only, muting is
// the amplifier enable pin, and volume is applied to the samples in software.
bool audio_begin();

// 0..100, applied to the PCM before it reaches I2S.
void audio_set_volume(uint8_t percent);
uint8_t audio_get_volume();

// Plays signed 16-bit little-endian mono samples at AUDIO_SAMPLE_RATE, pulled
// from `src` until `bytes` have been consumed or the stream dies. Blocking;
// call it from a task, never from the LVGL loop.
bool audio_play_pcm(Stream &src, size_t bytes);

// Short synthesised chirp, used to confirm the speaker works without needing
// the bridge to be reachable.
void audio_test_tone();

#define AUDIO_SAMPLE_RATE 16000
