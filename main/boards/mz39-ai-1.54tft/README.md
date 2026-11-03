# MZ39-AI 1.54" TFT (esp32c3)

ESP32-C3-based AI voice assistant board (silkscreen `MZ39-AI_V1.3`), sold as the
"Xiaozhi" AI voice assistant, with:

- 1.54" 240x240 ST7789 SPI LCD (`FPC-HJC-JCD-154` cable)
- VB6824 audio chip over UART (wake word "你好小智" and voice-command detection run
  on the chip itself; mic link is OPUS 16 kHz packets decoded on the ESP32,
  playback is plain 16 kHz PCM — `CONFIG_VB6824_TYPE_OPUS_16K_20MS_PCM_16K=y`)
- Boot button on GPIO9, volume up/down on GPIO7/GPIO8 (right/left)
- Optional 4G modem pad (empty by default)

## Pin map

Function                | GPIO
------------------------|-----
LCD SCLK / MOSI         | 3 / 1
LCD DC / CS / RST       | 0 / 12 / 2
LCD backlight (PWM)     | 5
VB6824 UART (TX/RX)     | 20 / 10 (UART1)
Boot / Vol- / Vol+      | 9 / 8 / 7

The pin map was verified on hardware and matches the vendor
`doit-ai-02-kit-lcd` configuration shipped in SmartArduino/DOIT_AI.

## Build

```sh
python3 scripts/build.py mz39-ai-1.54tft
```

Requires 8 MB flash (`partitions/v2/8m.csv`) — set by this board's
`config.json`. Wake word detection is handled by the VB6824 chip, so the
software wake word engine stays disabled (`CONFIG_WAKE_WORD_DISABLED=y`).

## VB6824 component

The `vb6824` component (vendored copy of `78/vb6824` v2.3.0 with the idf
constraint relaxed to `>=5.3`) lives in
`main/boards/mz39-ai-1.54tft/components/vb6824/` and is only pulled into the
build for this board via the `$CONFIG{BOARD_TYPE_MZ39_AI_1_54TFT}` rule in
`main/idf_component.yml`. `main/CMakeLists.txt` adds this board's codec source
(`main/audio/codecs/vb6824_audio_codec.cc`) and `vb6824` to the main component's
private requires under the same condition.

## Notes

- The displayed wake word string is configured on the VB6824; the firmware
  maps its voice command to `WakeWordInvoke("你好小智")`.
- No battery ADC monitoring yet (candidate pin: GPIO4).
