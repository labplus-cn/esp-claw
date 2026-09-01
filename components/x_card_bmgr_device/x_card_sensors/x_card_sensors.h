/*
 * SPDX-FileCopyrightText: 2026 X-Card
 * SPDX-License-Identifier: MIT
 *
 * X-Card I2C sensor drivers (ESP-IDF v5.5 new I2C driver).
 * All sensors share the i2c_master bus via esp_board_manager peripherals.
 *
 * Sensors:
 *   - QMI8658     6-axis IMU (Accel + Gyro)   7-bit I2C addr: 0x6B (107)
 *   - LTR-308ALS  Ambient Light Sensor        7-bit I2C addr: 0x29 (user addr 0x53 >> 1)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_board_manager_includes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 *  QMI8658 — 6-axis IMU (Accelerometer + Gyroscope)
 *  Datasheet 7-bit I2C address: 0x6B (AD0=1)
 * =================================================================== */

#define QMI8658_I2C_ADDR_7BIT    0x6B

#define QMI8658_REG_WHO_AM_I     0x00
#define QMI8658_WHO_AM_I_VAL     0x05

/** QMI8658 device handle (opaque — pass to read functions) */
typedef struct qmi8658_handle qmi8658_handle_t;

esp_err_t qmi8658_read_accel(qmi8658_handle_t *handle, float *x, float *y, float *z);
esp_err_t qmi8658_read_gyro(qmi8658_handle_t *handle, float *x, float *y, float *z);

int qmi8658_init(void *cfg, int cfg_size, void **device_handle);
int qmi8658_deinit(void *device_handle);

/* ===================================================================
 *  LTR-308ALS-01 — Ambient Light Sensor
 *  Datasheet 7-bit slave address = 0x53 (which is not an 8-bit read address).
 * =================================================================== */

#define LTR308ALS_I2C_ADDR_7BIT  0x53

#define LTR308ALS_REG_PART_ID    0x06
#define LTR308ALS_PART_ID_VAL    0xB1

typedef struct ltr308als_handle ltr308als_handle_t;

esp_err_t ltr308als_read_lux(ltr308als_handle_t *handle, float *lux);

int ltr308als_init(void *cfg, int cfg_size, void **device_handle);
int ltr308als_deinit(void *device_handle);

#ifdef __cplusplus
}
#endif
