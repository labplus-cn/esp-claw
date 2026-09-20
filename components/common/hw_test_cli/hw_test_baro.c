/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Production test for SPL06-001 barometric pressure sensor.
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

static const char *TAG = "test_baro";

/* SPL06-001 registers */
#define SPL06_REG_PSR_B2    0x00
#define SPL06_REG_TMP_B2    0x03
#define SPL06_REG_PRS_CFG   0x06
#define SPL06_REG_TMP_CFG   0x07
#define SPL06_REG_MEAS_CFG  0x08
#define SPL06_REG_CFG_REG   0x09
#define SPL06_REG_ID        0x0D
#define SPL06_REG_COEF_BASE 0x10

#define SPL06_CHIP_ID       0x11  /* SPL06-001 may report 0x10 or 0x11 */
#define SPL06_I2C_ADDR_LOW  0x76
#define SPL06_I2C_TIMEOUT_MS 100
#define SPL06_SETTLE_MS     200

/* MEAS_CFG(0x08) status bits (datasheet) */
#define SPL06_MEAS_COEF_RDY    0x80  /* bit7: calibration coefficients ready */
#define SPL06_MEAS_SENSOR_RDY  0x40  /* bit6: sensor initialization complete */
#define SPL06_MEAS_TMP_RDY     0x20  /* bit5: temperature data ready */
#define SPL06_MEAS_PRS_RDY     0x10  /* bit4: pressure data ready */
#define SPL06_MEAS_CTRL_BG_PT  0x07  /* bits[2:0]=111: continuous pressure + temperature */

/*
 * Config values per datasheet.
 * PM_PRC/TMP_PRC = 0011 -> 8x oversampling (NO P_SHIFT/T_SHIFT required).
 * Rate = 011 -> 8 measurements/s. TMP_EXT(bit7)=1 (MEMS on-chip temp sensor).
 * NOTE: PM_PRC=0101 would be 32x, which requires P_SHIFT/T_SHIFT in CFG_REG
 * and a different scale factor (516096). We intentionally stay at 8x.
 */
#define SPL06_PRS_CFG_8X   0x33  /* PM_RATE=011(8/s) | PM_PRC=0011(8x) */
#define SPL06_TMP_CFG_8X   0xB3  /* TMP_EXT=1 | TMP_RATE=011(8/s) | TMP_PRC=011(8x) */
#define SPL06_CFG_REG_NOSHIFT 0x00  /* no shift at 8x oversampling */

/* Scale factor for 8x oversampling (datasheet scaling-factor table) */
#define SPL06_SCALE_8X      7864320.0f

typedef struct {
    int16_t c0, c1;
    int32_t c00, c10;   /* 20-bit signed */
    int16_t c01, c11, c20, c21, c30;
} spl06_calib_t;

typedef struct {
    uint8_t i2c_addr;
    int samples;
} baro_test_args_t;

static void baro_parse_args(int argc, char **argv, baro_test_args_t *args)
{
    args->i2c_addr = SPL06_I2C_ADDR_LOW;
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

static esp_err_t baro_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), SPL06_I2C_TIMEOUT_MS);
}

static esp_err_t baro_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, SPL06_I2C_TIMEOUT_MS);
}

static esp_err_t spl06_read_calib(i2c_master_dev_handle_t dev, spl06_calib_t *c)
{
    /* Coefficients occupy registers 0x10..0x21 = 18 bytes */
    uint8_t coef[18] = { 0 };
    esp_err_t err = baro_read(dev, SPL06_REG_COEF_BASE, coef, 18);
    if (err != ESP_OK) return err;

    /* c0: 12-bit signed (coef[0] << 4 | coef[1] >> 4) */
    c->c0 = (int16_t)(((uint16_t)coef[0] << 4) | ((coef[1] >> 4) & 0x0F));
    if (c->c0 & 0x800) c->c0 |= 0xF000;
    /* c1: 12-bit signed (coef[1] low 4 bits << 8 | coef[2]) */
    c->c1 = (int16_t)(((uint16_t)(coef[1] & 0x0F) << 8) | coef[2]);
    if (c->c1 & 0x800) c->c1 |= 0xF000;
    /* c00: 20-bit signed (coef[3] << 12 | coef[4] << 4 | coef[5] >> 4) */
    c->c00 = (int32_t)(((uint32_t)coef[3] << 12) | ((uint32_t)coef[4] << 4) | ((coef[5] >> 4) & 0x0F));
    if (c->c00 & 0x80000) c->c00 |= 0xFFF00000;
    /* c10: 20-bit signed (coef[5] low 4 bits << 16 | coef[6] << 8 | coef[7]) */
    c->c10 = (int32_t)(((uint32_t)(coef[5] & 0x0F) << 16) | ((uint32_t)coef[6] << 8) | coef[7]);
    if (c->c10 & 0x80000) c->c10 |= 0xFFF00000;
    /* c01..c30: 16-bit signed */
    c->c01 = (int16_t)(((uint16_t)coef[8] << 8) | coef[9]);
    c->c11 = (int16_t)(((uint16_t)coef[10] << 8) | coef[11]);
    c->c20 = (int16_t)(((uint16_t)coef[12] << 8) | coef[13]);
    c->c21 = (int16_t)(((uint16_t)coef[14] << 8) | coef[15]);
    c->c30 = (int16_t)(((uint16_t)coef[16] << 8) | coef[17]);
    return ESP_OK;
}

int test_baro(int argc, char **argv)
{
    baro_test_args_t args;
    baro_parse_args(argc, argv, &args);

    printf("Barometer test: SPL06-001 @ 0x%02x, %d sample(s)\n",
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

    /* Chip ID check */
    uint8_t chip_id = 0;
    err = baro_read(dev, SPL06_REG_ID, &chip_id, 1);
    if (err != ESP_OK || chip_id != SPL06_CHIP_ID) {
        printf("[FAIL] Chip ID = 0x%02x (expect 0x%02x)\n", chip_id, SPL06_CHIP_ID);
        goto cleanup;
    }
    printf("  Chip ID = 0x%02x ... OK\n", chip_id);

    /* Configure per datasheet: 8x oversampling (no shift), continuous P+T */
    baro_write(dev, SPL06_REG_PRS_CFG, SPL06_PRS_CFG_8X);
    baro_write(dev, SPL06_REG_TMP_CFG, SPL06_TMP_CFG_8X);
    baro_write(dev, SPL06_REG_CFG_REG, SPL06_CFG_REG_NOSHIFT);

    /* Wait for SENSOR_RDY and COEF_RDY before reading coefficients */
    uint8_t meas = 0;
    int timeout = 100;
    do {
        baro_read(dev, SPL06_REG_MEAS_CFG, &meas, 1);
        if ((meas & (SPL06_MEAS_SENSOR_RDY | SPL06_MEAS_COEF_RDY)) ==
            (SPL06_MEAS_SENSOR_RDY | SPL06_MEAS_COEF_RDY)) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (--timeout > 0);
    printf("  MEAS_CFG status=0x%02X (SENSOR_RDY|COEF_RDY %s)\n", meas,
           ((meas & 0xC0) == 0xC0) ? "OK" : "NOT READY");

    /* Read calibration */
    spl06_calib_t calib;
    err = spl06_read_calib(dev, &calib);
    if (err != ESP_OK) {
        printf("[FAIL] Calibration read error: %s\n", esp_err_to_name(err));
        goto cleanup;
    }
    printf("  Calibration loaded\n");
    printf("  c0=%d c1=%d c00=%ld c10=%ld c01=%d c11=%d c20=%d c21=%d c30=%d\n",
           calib.c0, calib.c1, (long)calib.c00, (long)calib.c10,
           calib.c01, calib.c11, calib.c20, calib.c21, calib.c30);

    /* Start continuous background measurement (pressure + temperature) */
    baro_write(dev, SPL06_REG_MEAS_CFG, SPL06_MEAS_CTRL_BG_PT);
    vTaskDelay(pdMS_TO_TICKS(SPL06_SETTLE_MS));

    printf("  Reading %d sample(s):\n", args.samples);
    int good_samples = 0;

    for (int s = 0; s < args.samples; s++) {
        uint8_t buf[3] = { 0 };

        /* Wait for both pressure and temperature data ready */
        timeout = 100;
        do {
            baro_read(dev, SPL06_REG_MEAS_CFG, &meas, 1);
            if ((meas & (SPL06_MEAS_PRS_RDY | SPL06_MEAS_TMP_RDY)) ==
                (SPL06_MEAS_PRS_RDY | SPL06_MEAS_TMP_RDY)) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        } while (--timeout > 0);
        if ((meas & (SPL06_MEAS_PRS_RDY | SPL06_MEAS_TMP_RDY)) !=
            (SPL06_MEAS_PRS_RDY | SPL06_MEAS_TMP_RDY)) {
            printf("    #%d  [TIMEOUT] data not ready (meas=0x%02X)\n", s + 1, meas);
            continue;
        }

        /* Read temperature (24-bit signed) */
        err = baro_read(dev, SPL06_REG_TMP_B2, buf, 3);
        if (err != ESP_OK) { printf("    #%d  [FAIL] temp read\n", s + 1); continue; }
        int32_t raw_t = (int32_t)((buf[0] << 16) | (buf[1] << 8) | buf[2]);
        if (raw_t & 0x800000) raw_t |= 0xFF000000;

        /* Read pressure (24-bit signed) */
        err = baro_read(dev, SPL06_REG_PSR_B2, buf, 3);
        if (err != ESP_OK) { printf("    #%d  [FAIL] pres read\n", s + 1); continue; }
        int32_t raw_p = (int32_t)((buf[0] << 16) | (buf[1] << 8) | buf[2]);
        if (raw_p & 0x800000) raw_p |= 0xFF000000;

        /* Compensate (8x oversampling scale factors) */
        float Traw_sc = (float)raw_t / SPL06_SCALE_8X;
        float Praw_sc = (float)raw_p / SPL06_SCALE_8X;
        float Tcomp = calib.c0 * 0.5f + calib.c1 * Traw_sc;
        float Pcomp = calib.c00
                      + Praw_sc * (calib.c10 + Praw_sc * (calib.c20 + Praw_sc * calib.c30))
                      + Traw_sc * calib.c01
                      + Traw_sc * Praw_sc * (calib.c11 + Praw_sc * calib.c21);

        printf("    #%d  raw_t=%ld raw_p=%ld  Temp: %.2f C  Pressure: %.1f Pa (%.2f hPa)\n",
               s + 1, (long)raw_t, (long)raw_p, Tcomp, Pcomp, Pcomp / 100.0f);

        if (Tcomp > -40.0f && Tcomp < 85.0f && Pcomp > 30000.0f && Pcomp < 125000.0f) {
            good_samples++;
        }
    }

    pass = (good_samples > 0);
    printf("\n=== Barometer test %s (%d/%d valid sample(s)) ===\n",
           pass ? "PASSED" : "FAILED", good_samples, args.samples);

cleanup:
    i2c_master_bus_rm_device(dev);
    return 0;
}
