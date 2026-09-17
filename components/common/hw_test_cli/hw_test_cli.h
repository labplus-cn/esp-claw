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
