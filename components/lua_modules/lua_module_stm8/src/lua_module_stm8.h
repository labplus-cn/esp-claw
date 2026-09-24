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
 * @brief Register the "stm8" Lua module for STM8S001 slave MCU control.
 *
 * Provides Lua API: require("stm8").new({ device = "stm8s001" })
 * Reads I2C config from board manager YAML when device name is given.
 */
esp_err_t lua_module_stm8_register(void);

#ifdef __cplusplus
}
#endif
