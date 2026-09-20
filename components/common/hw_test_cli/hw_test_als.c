/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Production test for LTR-308ALS ambient light sensor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_als";

/* LTR-308ALS registers */
#define LTR_REG_CTRL        0x00
#define LTR_REG_MEAS_RATE   0x04
#define LTR_REG_GAIN        0x05
#define LTR_REG_PART_ID     0x06
#define LTR_REG_STATUS      0x07
#define LTR_REG_ALS_DATA0   0x0D

#define LTR_CTRL_ACTIVE     0x02
#define LTR_CTRL_RESET      0x10
#define LTR_MEAS_100MS      0x22
#define LTR_STATUS_DRDY     0x08
#define LTR_PART_ID_VAL     0xB1

#define LTR_I2C_ADDR        0x53
#define LTR_I2C_TIMEOUT_MS  100
#define LTR_SETTLE_MS       150

static const float ltr_gains[] = { 1.0f, 3.0f, 6.0f, 9.0f, 18.0f };

typedef struct {
    uint8_t i2c_addr;
    int samples;
} als_test_args_t;

static void als_parse_args(int argc, char **argv, als_test_args_t *args)
{
    args->i2c_addr = LTR_I2C_ADDR;
    args->samples = 5;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            args->i2c_addr = (uint8_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            args->samples = atoi(argv[++i]);
            if (args->samples < 1) args->samples = 1;
        }
    }
}

static esp_err_t als_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), LTR_I2C_TIMEOUT_MS);
}

static esp_err_t als_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, LTR_I2C_TIMEOUT_MS);
}

int test_als(int argc, char **argv)
{
    als_test_args_t args;
    als_parse_args(argc, argv, &args);

    printf("ALS test: LTR-308ALS @ 0x%02x, %d sample(s)\n",
           args.i2c_addr, args.samples);

    void *bus_raw = NULL;
    esp_err_t err = esp_board_manager_get_periph_handle("i2c_master", &bus_raw);
    if (err != ESP_OK) {
        printf("[FAIL] I2C bus not available: %s\n", esp_err_to_name(err));
        return 0;
    }
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bus_raw;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = args.i2c_addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        printf("[FAIL] Cannot add I2C device: %s\n", esp_err_to_name(err));
        return 0;
    }

    bool pass = false;

    /* Part ID check */
    uint8_t pid = 0;
    err = als_read(dev, LTR_REG_PART_ID, &pid, 1);
    if (err != ESP_OK || pid != LTR_PART_ID_VAL) {
        printf("[FAIL] Part ID = 0x%02x (expect 0x%02x)\n", pid, LTR_PART_ID_VAL);
        goto cleanup;
    }
    printf("  Part ID = 0x%02x ... OK\n", pid);

    /* Initialize: reset, configure, activate */
    als_write(dev, LTR_REG_CTRL, LTR_CTRL_RESET);
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t gain_idx = 1;  /* 3x gain */
    als_write(dev, LTR_REG_GAIN, gain_idx);
    als_write(dev, LTR_REG_MEAS_RATE, LTR_MEAS_100MS);
    als_write(dev, LTR_REG_CTRL, LTR_CTRL_ACTIVE);
    vTaskDelay(pdMS_TO_TICKS(LTR_SETTLE_MS));

    printf("  Reading %d sample(s):\n", args.samples);
    int good_samples = 0;

    for (int s = 0; s < args.samples; s++) {
        uint8_t status = 0;
        als_read(dev, LTR_REG_STATUS, &status, 1);

        if (!(status & LTR_STATUS_DRDY)) {
            printf("    #%d  [NOT READY] status=0x%02x\n", s + 1, status);
            vTaskDelay(pdMS_TO_TICKS(110));  /* Measurement rate = 100ms */
            continue;
        }

        uint8_t raw[3] = { 0 };
        err = als_read(dev, LTR_REG_ALS_DATA0, raw, 3);
        if (err != ESP_OK) {
            printf("    #%d  [FAIL] read error: %s\n", s + 1, esp_err_to_name(err));
            continue;
        }

        uint32_t als_raw = raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16);
        float lux = als_raw * 0.6f / ltr_gains[gain_idx];

        printf("    #%d  raw=%u  Lux: %.1f\n", s + 1, (unsigned)als_raw, lux);

        if (lux >= 0.0f) good_samples++;
        
        /* Wait for next measurement cycle (100ms rate) */
        vTaskDelay(pdMS_TO_TICKS(110));
    }

    pass = (good_samples > 0);
    printf("\n=== ALS test %s (%d/%d valid sample(s)) ===\n",
           pass ? "PASSED" : "FAILED", good_samples, args.samples);

cleanup:
    i2c_master_bus_rm_device(dev);
    return 0;
}
