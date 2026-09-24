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
 * @brief Register the "usb_otg" Lua module for USB OTG role switching.
 *
 * Provides Lua API:
 *   local otg = require("usb_otg")
 *   otg.status()           -- returns "none", "host", or "device"
 *   otg.start_host()       -- switch to Host mode (UVC camera)
 *   otg.start_device()     -- switch to Device mode (MSC U-disk)
 *   otg.stop()             -- stop current role
 */
esp_err_t lua_module_usb_otg_register(void);

#ifdef __cplusplus
}
#endif
