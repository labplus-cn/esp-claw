/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hw_test_usb_camera_start(void);
esp_err_t hw_test_usb_camera_stop(void);
bool hw_test_usb_camera_is_running(void);

#ifdef __cplusplus
}
#endif

