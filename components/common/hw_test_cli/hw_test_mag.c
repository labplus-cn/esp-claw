/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Production test for MMC5603NJ magnetometer.
 * Based on official datasheet Rev.B
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_mag";

/* MMC5603 registers (from datasheet) */
#define MMC5603_REG_PRODUCT_ID   0x39
#define MMC5603_PRODUCT_ID_VAL   0x10
#define MMC5603_REG_STATUS1      0x18
#define MMC5603_STATUS_OTP_DONE  0x10  /* Bit 4 */
#define MMC5603_STATUS_MEAS_DONE 0x40  /* Bit 6 - MM_DONE (measurement complete) */
#define MMC5603_REG_XOUT0        0x00
#define MMC5603_REG_CTRL0        0x1B
#define MMC5603_REG_CTRL1        0x1C
#define MMC5603_CTRL0_TMM        0x01  /* Take measurement */
#define MMC5603_CTRL0_SET        0x08  /* Do SET */
#define MMC5603_CTRL0_AUTO_SR    0x20  /* Auto SET/RESET */

#define MMC5603_I2C_ADDR         0x30
#define MMC5603_I2C_TIMEOUT_MS   100

typedef struct {
    uint8_t i2c_addr;
    int samples;
} mag_test_args_t;

static void mag_parse_args(int argc, char **argv, mag_test_args_t *args)
{
    args->i2c_addr = MMC5603_I2C_ADDR;
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

static esp_err_t mag_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), MMC5603_I2C_TIMEOUT_MS);
}

static esp_err_t mag_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, MMC5603_I2C_TIMEOUT_MS);
}

int test_mag(int argc, char **argv)
{
    mag_test_args_t args;
    mag_parse_args(argc, argv, &args);

    printf("Magnetometer test: MMC5603 @ 0x%02x, %d sample(s)\n",
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

    /* Step 1: Wait for power up (5ms) */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Step 2: Wait for OTP_read_done */
    printf("  Waiting for OTP done...\n");
    uint8_t status = 0;
    int timeout = 100;
    do {
        mag_read(dev, MMC5603_REG_STATUS1, &status, 1);
        if (status & MMC5603_STATUS_OTP_DONE) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (--timeout > 0);
    
    if (!(status & MMC5603_STATUS_OTP_DONE)) {
        printf("[FAIL] OTP not ready (status=0x%02X)\n", status);
        goto cleanup;
    }
    printf("  OTP done (status=0x%02X)\n", status);

    /* Step 3: Verify Product ID */
    uint8_t whoami = 0;
    err = mag_read(dev, MMC5603_REG_PRODUCT_ID, &whoami, 1);
    if (err != ESP_OK || whoami != MMC5603_PRODUCT_ID_VAL) {
        printf("[FAIL] Product ID = 0x%02x (expect 0x%02x)\n", whoami, MMC5603_PRODUCT_ID_VAL);
        goto cleanup;
    }
    printf("  Product ID = 0x%02x ... OK\n", whoami);

    /* Step 4: Enable Auto SET/RESET */
    mag_write(dev, MMC5603_REG_CTRL0, MMC5603_CTRL0_AUTO_SR);
    vTaskDelay(pdMS_TO_TICKS(10));

    printf("  Reading %d sample(s):\n", args.samples);
    int good_samples = 0;

    for (int s = 0; s < args.samples; s++) {
        /* Trigger single measurement */
        mag_write(dev, MMC5603_REG_CTRL0, MMC5603_CTRL0_TMM | MMC5603_CTRL0_AUTO_SR);
        
        /* Wait for measurement done (DRDY) */
        timeout = 200;
        do {
            mag_read(dev, MMC5603_REG_STATUS1, &status, 1);
            if (status & MMC5603_STATUS_MEAS_DONE) break;
            vTaskDelay(pdMS_TO_TICKS(1));
        } while (--timeout > 0);

        if (!(status & MMC5603_STATUS_MEAS_DONE)) {
            printf("    #%d  [TIMEOUT] status=0x%02X\n", s + 1, status);
            continue;
        }

        /* Read 9 bytes of 20-bit data (X0,X1,Y0,Y1,Z0,Z1,X2,Y2,Z2) */
        uint8_t data[9] = { 0 };
        err = mag_read(dev, MMC5603_REG_XOUT0, data, 9);
        if (err != ESP_OK) {
            printf("    #%d  [FAIL] read error: %s\n", s + 1, esp_err_to_name(err));
            continue;
        }

        /* Parse 20-bit UNSIGNED values (MSB first), null-field = 524288 */
        uint32_t raw_x = (uint32_t)((data[0] << 12) | (data[1] << 4) | (data[6] >> 4));
        uint32_t raw_y = (uint32_t)((data[2] << 12) | (data[3] << 4) | (data[7] >> 4));
        uint32_t raw_z = (uint32_t)((data[4] << 12) | (data[5] << 4) | (data[8] >> 4));

        /* Convert to Gauss: (raw - 524288) / 16384 */
        float gx = ((float)raw_x - 524288.0f) / 16384.0f;
        float gy = ((float)raw_y - 524288.0f) / 16384.0f;
        float gz = ((float)raw_z - 524288.0f) / 16384.0f;

        printf("    #%d  Mag: %+7.3f %+7.3f %+7.3f G\n", s + 1, gx, gy, gz);

        /* Check if data is reasonable (earth's field ~0.25-0.65 G) */
        float mag = gx*gx + gy*gy + gz*gz;
        if (mag > 0.01f && mag < 100.0f) good_samples++;
    }

    pass = (good_samples > 0);
    printf("\n=== Magnetometer test %s (%d/%d valid sample(s)) ===\n",
           pass ? "PASSED" : "FAILED", good_samples, args.samples);

cleanup:
    i2c_master_bus_rm_device(dev);
    return 0;
}
