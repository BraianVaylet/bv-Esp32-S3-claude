# es8311 (vendored)

ES8311 audio codec driver, **not written for this project**.

- Copyright 2015-2022 Espressif Systems (Shanghai) CO LTD
- SPDX-License-Identifier: **Apache-2.0**

Taken unmodified from Waveshare's Arduino demo for this exact board:
`examples/ESP32-S3-Touch-LCD-1.54-demo/Arduino-3.2.0/examples/01_i2s_audio/`
in [waveshareteam/ESP32-S3-Touch-LCD-1.54](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54).

It is vendored rather than pulled from the registry because this variant is
patched to do its register access through Arduino's `Wire` instead of the
ESP-IDF I2C driver — which is exactly why `audio_begin()` has to run before
`display_begin()` and release the bus afterwards. See the note in
`include/audio.h`.
