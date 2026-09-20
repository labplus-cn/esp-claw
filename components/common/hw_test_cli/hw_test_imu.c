/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Production test for QMI8658 6-axis IMU.
 *
 * Directly talks to the sensor over I2C via the board-manager bus handle;
 * does NOT depend on lua_module_imu or the Waveshare QMI8658 component.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_imu";

/* ---- QMI8658 register map (subset needed for production test) ---- */
#define QMI8658_REG_WHO_AM_I   0x00
#define QMI8658_WHO_AM_I_VAL   0x05
#define QMI8658_REG_CTRL1      0x02
#define QMI8658_REG_CTRL2      0x03
#define QMI8658_REG_CTRL3      0x04
#define QMI8658_REG_CTRL7      0x07   /* NOTE: Waveshare component wrongly defines this as 0x08 */
#define QMI8658_REG_ACC_X_L    0x35
#define QMI8658_REG_GYR_X_L    0x3B

#define QMI8658_CTRL7_ACC_EN   0x01
#define QMI8658_CTRL7_GYR_EN   0x02
#define QMI8658_CTRL1_ADDR_AI  0x40   /* Auto-increment address */
#define QMI8658_CTRL1_SW_RESET 0x80   /* Software reset via CTRL1 */
#define QMI8658_CTRL2_16G_1K   0x33   /* ±16 g, 1000 Hz */
#define QMI8658_CTRL3_2048_1K  0x73   /* ±2048 dps, 1000 Hz */

#define QMI8658_ACC_SENS_16G   (16.0f / 32768.0f)  /* LSB → g */
#define QMI8658_GYR_SENS_2048  (2048.0f / 32768.0f) /* LSB → dps */

#define QMI8658_I2C_TIMEOUT_MS 100
#define QMI8658_DEFAULT_ADDR   0x6B
#define QMI8658_SETTLE_MS      100  /* Sensor startup after enable */

/* ---- I2C helpers (same pattern as x_card_sensors.c) ---- */

static esp_err_t imu_reg_write(i2c_master_dev_handle_t dev,
                               uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), QMI8658_I2C_TIMEOUT_MS);
}

static esp_err_t imu_reg_read(i2c_master_dev_handle_t dev,
                              uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len,
                                       QMI8658_I2C_TIMEOUT_MS);
}

/* ---- Argument parsing ---- */

typedef struct {
    uint8_t i2c_addr;
    int     samples;
} imu_test_args_t;

static void imu_parse_args(int argc, char **argv, imu_test_args_t *args)
{
    args->i2c_addr = QMI8658_DEFAULT_ADDR;
    args->samples  = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: test imu [options]\n");
            printf("  --addr 0x6B   QMI8658 I2C address (default: 0x%02x)\n",
                   QMI8658_DEFAULT_ADDR);
            printf("  --samples N   Number of samples to read (default: 5)\n");
            return;
        }
        if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            args->i2c_addr = (uint8_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            args->samples = atoi(argv[++i]);
            if (args->samples < 1) {
                args->samples = 1;
            }
        }
    }
}

/* ---- Main test entry point ---- */

int test_imu(int argc, char **argv)
{
    imu_test_args_t args;
    imu_parse_args(argc, argv, &args);

    printf("IMU test: QMI8658 @ 0x%02x, %d sample(s)\n",
           args.i2c_addr, args.samples);

    /* 1. Get I2C bus from board manager */
    void *bus_raw = NULL;
    esp_err_t err = esp_board_manager_get_periph_handle("i2c_master", &bus_raw);
    if (err != ESP_OK) {
        printf("[FAIL] I2C bus 'i2c_master' not available: %s\n",
               esp_err_to_name(err));
        return 0;
    }
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bus_raw;

    /* 2. Add device on the shared bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = args.i2c_addr,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        printf("[FAIL] Cannot add I2C device 0x%02x: %s\n",
               args.i2c_addr, esp_err_to_name(err));
        return 0;
    }

    bool pass = false;

    /* 3. WHO_AM_I check */
    uint8_t whoami = 0;
    err = imu_reg_read(dev, QMI8658_REG_WHO_AM_I, &whoami, 1);
    if (err != ESP_OK) {
        printf("[FAIL] WHO_AM_I read error: %s\n", esp_err_to_name(err));
        goto cleanup;
    }
    printf("  WHO_AM_I = 0x%02x (expect 0x%02x) ... %s\n",
           whoami, QMI8658_WHO_AM_I_VAL,
           (whoami == QMI8658_WHO_AM_I_VAL) ? "OK" : "MISMATCH");
    if (whoami != QMI8658_WHO_AM_I_VAL) {
        printf("[FAIL] Chip ID mismatch\n");
        goto cleanup;
    }

    /* 4. Configure and enable sensors.
     *    The QMI8658 CTRL7 register address varies between silicon revisions:
     *    some use 0x07, others 0x08.  Write to both to cover all variants. */
    imu_reg_write(dev, QMI8658_REG_CTRL1, QMI8658_CTRL1_ADDR_AI);
    imu_reg_write(dev, QMI8658_REG_CTRL2, QMI8658_CTRL2_16G_1K);
    imu_reg_write(dev, QMI8658_REG_CTRL3, QMI8658_CTRL3_2048_1K);
    imu_reg_write(dev, 0x07, QMI8658_CTRL7_ACC_EN | QMI8658_CTRL7_GYR_EN);
    imu_reg_write(dev, 0x08, QMI8658_CTRL7_ACC_EN | QMI8658_CTRL7_GYR_EN);

    /* Sensor needs ~50-100 ms after enable before producing valid data */
    vTaskDelay(pdMS_TO_TICKS(QMI8658_SETTLE_MS));

    /* 5. Read samples */
    printf("  Reading %d sample(s):\n", args.samples);
    int good_samples = 0;

    for (int s = 0; s < args.samples; s++) {
        uint8_t raw[6];

        /* Accelerometer: 6 bytes starting at ACC_X_L */
        err = imu_reg_read(dev, QMI8658_REG_ACC_X_L, raw, 6);
        if (err != ESP_OK) {
            printf("    #%d  [FAIL] accel read error: %s\n",
                   s + 1, esp_err_to_name(err));
            continue;
        }
        int16_t ax = (int16_t)(raw[0] | (raw[1] << 8));
        int16_t ay = (int16_t)(raw[2] | (raw[3] << 8));
        int16_t az = (int16_t)(raw[4] | (raw[5] << 8));

        /* Gyroscope: 6 bytes starting at GYR_X_L */
        err = imu_reg_read(dev, QMI8658_REG_GYR_X_L, raw, 6);
        if (err != ESP_OK) {
            printf("    #%d  [FAIL] gyro read error: %s\n",
                   s + 1, esp_err_to_name(err));
            continue;
        }
        int16_t gx = (int16_t)(raw[0] | (raw[1] << 8));
        int16_t gy = (int16_t)(raw[2] | (raw[3] << 8));
        int16_t gz = (int16_t)(raw[4] | (raw[5] << 8));

        float ax_g = ax * QMI8658_ACC_SENS_16G;
        float ay_g = ay * QMI8658_ACC_SENS_16G;
        float az_g = az * QMI8658_ACC_SENS_16G;
        float gx_d = gx * QMI8658_GYR_SENS_2048;
        float gy_d = gy * QMI8658_GYR_SENS_2048;
        float gz_d = gz * QMI8658_GYR_SENS_2048;

        printf("    #%d  Accel: %+7.2f %+7.2f %+7.2f g"
               "  |  Gyro: %+8.2f %+8.2f %+8.2f dps\n",
               s + 1, ax_g, ay_g, az_g, gx_d, gy_d, gz_d);

        /* Sanity: raw data should not be all-zero or all-0xFF */
        int16_t acc_abs[3] = { abs(ax), abs(ay), abs(az) };
        int max_abs = acc_abs[0];
        if (acc_abs[1] > max_abs) { max_abs = acc_abs[1]; }
        if (acc_abs[2] > max_abs) { max_abs = acc_abs[2]; }
        if (max_abs > 100) {
            good_samples++;
        }
    }

    /* 6. Disable sensors (both possible CTRL7 addresses) */
    imu_reg_write(dev, 0x07, 0x00);
    imu_reg_write(dev, 0x08, 0x00);

    /* 7. Result */
    pass = (good_samples > 0);
    printf("\n=== IMU test %s (%d/%d valid sample(s)) ===\n",
           pass ? "PASSED" : "FAILED", good_samples, args.samples);

cleanup:
    i2c_master_bus_rm_device(dev);
    return 0;
}
