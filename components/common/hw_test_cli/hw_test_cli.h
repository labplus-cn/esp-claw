/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register hardware test CLI commands with esp_console.
 *
 * Registers a single ``test`` command that dispatches to sub-commands:
 *   test i2c            -- scan I2C bus for devices
 *   test screen         -- draw RGB color blocks on LCD
 *   test sdcard         -- write / read / verify a temp file
 *   test camera         -- start camera preview on LCD
 *   test camera stop    -- stop camera preview
 *   test audio          -- record mic then play back through speaker
 *   test touch          -- touch panel test (background, touch to mark points)
 *   test touch stop     -- stop touch test
 *   test lvgl           -- LVGL widget touch test (background)
 *   test lvgl stop      -- stop LVGL test
 *   test button         -- confirm button GPIO press test
 *   test button --gpio 35 --active 0 --timeout 10
 *   test imu            -- QMI8658 IMU I2C read test
 *   test imu --addr 0x6b --samples 10
 *   test mag            -- MMC5603 magnetometer test
 *   test mag --addr 0x30 --samples 10
 *   test als            -- LTR-308ALS ambient light sensor test
 *   test als --addr 0x29 --samples 10
 *   test baro           -- SPL06-001 barometric pressure sensor test
 *   test baro --addr 0x76 --samples 10
 *   test pwm            -- LEDC PWM pin connectivity test (start/stop)
 *   test pwm start      -- start 1 Hz PWM blink on IO20-23,51-54,45-48
 *   test pwm stop       -- stop PWM and turn off outputs
 *   test rfid           -- RC522 RFID reader I2C one-shot test
 *   test rfid start     -- enable antenna, background card polling
 *   test rfid stop      -- disable antenna, stop polling
 *   test rfid --addr 0x2F
 *   test usb            -- USB VBUS presence detect (reads GPIO from board mgr 'vbus_detector')
 *   test usb --monitor  -- continuous plug/unplug event monitor
 *   test usb --gpio 14 --active 1 --timeout 30  -- override board config
 *   test mcu            -- STM8S001 slave MCU (I2C 0x11)
 *   test mcu battery    -- read battery voltage (mV)
 *   test mcu motor1 <speed>  -- set motor 1 speed (-100..+100)
 *   test mcu motor2 <speed>  -- set motor 2 speed (-100..+100)
 *   test mcu stop       -- stop both motors
 *   test usb_otg        -- USB OTG role switching
 *   test usb_otg status -- show current role (none/host/device)
 *   test usb_otg host   -- switch to Host mode (UVC camera)
 *   test usb_otg device -- switch to Device mode (MSC U-disk)
 *   test usb_otg camera -- display an external UVC camera on the LCD
 *   test usb_otg camera stop -- stop UVC preview and release Host mode
 *   test usb_otg stop   -- stop current role, release USB
 *
 * File upload (lua_tool.c):
 *   fup <filename>      -- start upload (file goes to data root, e.g. /sdcard)
 *   fchunk <base64>     -- send a base64-encoded data chunk
 *   fup done            -- finish upload, close file
 *   lua --run --path /sdcard/main.lua         -- run script (sync)
 *   lua --run-async --path /sdcard/main.lua   -- run script (async)
 *
 * Adding a new test:
 *   1. Implement  int test_xxx(int argc, char **argv)  in a .c file.
 *   2. Add an entry to  s_sub_commands[]  in  hw_test_cli.c.
 *
 * @return ESP_OK on success
 */
esp_err_t hw_test_cli_init(void);

#ifdef __cplusplus
}
#endif
