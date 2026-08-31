# Labplus mPython V3

## Hardware profile

| Function | Configuration |
|---|---|
| MCU | ESP32-S3 |
| Flash | 16 MB QIO, 80 MHz |
| PSRAM | Octal, 80 MHz |
| Audio codec | ES8388, I2C address `0x20` |
| LCD | JD9853, 320 × 172, SPI2 |
| RGB status LEDs | WS2812 × 3, GPIO8 |
| Buttons | A: GPIO0, B: GPIO46 |

The audio, display and peripheral pin assignments are based on the existing
mPython V3 Board Manager definition in the Labplus ESP32-S3 firmware.

## Build

From the ESP-Claw repository:

```bash
cd application/edge_agent
idf.py bmgr -c ./boards -b mpython_v3
idf.py build
```

The Board Manager command must be rerun after changing any YAML board
definition. Generated files under `components/gen_bmgr_codes` are build
outputs and must not be edited manually.

## Hardware validation

The repository configuration has been prepared for static generation and
compilation. LCD orientation, audio capture/playback, buttons, RGB LEDs and
the actual flash/PSRAM module marking still require validation on a physical
mPython V3 board before flashing production devices.
