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
 * @brief Register the RFID capability group (rc522).
 *
 * Exposes the following LLM-callable tools:
 *   - rfid_scan:   Scan for RFID cards (blocking, returns card info)
 *   - rfid_stop:   Stop the RFID scanner
 *   - rfid_info:   Get RC522 chip information
 */
esp_err_t cap_rfid_register_group(void);

#ifdef __cplusplus
}
#endif
