/*
 * SPDX-FileCopyrightText: 2026 X-Card
 * SPDX-License-Identifier: MIT
 *
 * I2C sensor drivers for X-Card (ESP-IDF v5.5 new I2C driver).
 *
 * I2C model:  i2c_master_bus (board_manager) -> i2c_master_dev (per sensor)
 *
 * All i2c_master_{transmit,transmit_receive}() calls use dev_handle,
 * NOT the raw bus handle.  See esp-gmf test_periph_i2c.c for pattern.
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "x_card_sensors.h"

#define X_CARD_SENSOR_I2C_TIMEOUT_MS 100

static const char *TAG = "x_sensors";

/* -------------------------------------------------------------------
 *  Get the I2C bus that board_manager already created (port 0, 44/43)
 * ------------------------------------------------------------------- */

static esp_err_t sensor_get_bus(i2c_master_bus_handle_t *bus)
{
    void *raw = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle("i2c_master", &raw);
    if (ret != ESP_OK || !raw) {
        ESP_LOGE(TAG, "get i2c_master peripheral: %s", esp_err_to_name(ret));
        return ret != ESP_OK ? ret : ESP_ERR_NOT_FOUND;
    }
    *bus = (i2c_master_bus_handle_t)raw;
    return ESP_OK;
}

/* -------------------------------------------------------------------
 *  Add a device on the shared I2C bus -> returns i2c_master_dev_handle_t
 * ------------------------------------------------------------------- */

static esp_err_t sensor_add_dev(i2c_master_bus_handle_t bus, uint8_t addr_7bit,
                                i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_7bit,
        .scl_speed_hz    = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device at 0x%02X: %s", addr_7bit, esp_err_to_name(ret));
    }
    return ret;
}

/* -------------------------------------------------------------------
 *  Register I/O helpers — all take i2c_master_dev_handle_t
 * ------------------------------------------------------------------- */

static esp_err_t reg_write8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), X_CARD_SENSOR_I2C_TIMEOUT_MS);
}

static esp_err_t reg_read8(i2c_master_dev_handle_t dev, uint8_t reg,
                           uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len,
                                       X_CARD_SENSOR_I2C_TIMEOUT_MS);
}

/* ===================================================================
 *  QMI8658 — 6-axis IMU  (7-bit addr 0x6B)
 * =================================================================== */

struct qmi8658_handle {
    i2c_master_dev_handle_t i2c_dev;
};

#define QMI8658_REG_CTRL1       0x02
#define QMI8658_REG_CTRL2       0x03
#define QMI8658_REG_CTRL3       0x04
#define QMI8658_REG_CTRL7       0x08
#define QMI8658_REG_ACC_X_L     0x35
#define QMI8658_REG_GYR_X_L     0x3B

#define QMI8658_CTRL7_ACC_EN    0x01
#define QMI8658_CTRL7_GYR_EN    0x02
#define QMI8658_CTRL1_ADDR_AI   0x40  // 开启地址自动递增
#define QMI8658_CTRL2_16G_1K    0x33  // 加速度 ±16g, 1000Hz
#define QMI8658_CTRL3_2048_1K   0x73  // 陀螺仪 ±2048dps, 1000Hz

#define QMI8658_ACC_SENS_16G    (16.0f / 32768.0f)
#define QMI8658_GYR_SENS_2048   (2048.0f / 32768.0f)

int qmi8658_init(void *cfg, int cfg_size, void **device_handle)
{
    ESP_LOGI(TAG, "QMI8658: init (7-bit addr 0x%02X)", QMI8658_I2C_ADDR_7BIT);

    i2c_master_bus_handle_t bus;
    if (sensor_get_bus(&bus) != ESP_OK) return -1;

    i2c_master_dev_handle_t dev;
    if (sensor_add_dev(bus, QMI8658_I2C_ADDR_7BIT, &dev) != ESP_OK) return -1;

    /* Verify WHO_AM_I */
    uint8_t whoami = 0;
    reg_read8(dev, QMI8658_REG_WHO_AM_I, &whoami, 1);
    ESP_LOGI(TAG, "QMI8658: WHO_AM_I = 0x%02X (expect 0x%02X)", whoami, QMI8658_WHO_AM_I_VAL);

    /* Configure */
    reg_write8(dev, QMI8658_REG_CTRL1, QMI8658_CTRL1_ADDR_AI);
    reg_write8(dev, QMI8658_REG_CTRL2, QMI8658_CTRL2_16G_1K);
    reg_write8(dev, QMI8658_REG_CTRL3, QMI8658_CTRL3_2048_1K);
    reg_write8(dev, QMI8658_REG_CTRL7, QMI8658_CTRL7_ACC_EN | QMI8658_CTRL7_GYR_EN);

    qmi8658_handle_t *hdl = calloc(1, sizeof(*hdl));
    if (!hdl) return -1;
    hdl->i2c_dev = dev;
    *device_handle = hdl;

    ESP_LOGI(TAG, "QMI8658: ready");
    return 0;
}

int qmi8658_deinit(void *device_handle)
{
    if (!device_handle) return 0;
    qmi8658_handle_t *hdl = (qmi8658_handle_t *)device_handle;
    i2c_master_bus_rm_device(hdl->i2c_dev);
    free(hdl);
    ESP_LOGI(TAG, "QMI8658: deinit");
    return 0;
}

esp_err_t qmi8658_read_accel(qmi8658_handle_t *hdl, float *x, float *y, float *z)
{
    if (!hdl || !x || !y || !z) return ESP_ERR_INVALID_ARG;

    uint8_t raw[6];
    if (reg_read8(hdl->i2c_dev, QMI8658_REG_ACC_X_L, raw, 6) != ESP_OK) return -1;

    int16_t ax = (int16_t)(raw[0] | (raw[1] << 8));
    int16_t ay = (int16_t)(raw[2] | (raw[3] << 8));
    int16_t az = (int16_t)(raw[4] | (raw[5] << 8));

    *x = ax * QMI8658_ACC_SENS_16G;
    *y = ay * QMI8658_ACC_SENS_16G;
    *z = az * QMI8658_ACC_SENS_16G;
    return ESP_OK;
}

esp_err_t qmi8658_read_gyro(qmi8658_handle_t *hdl, float *x, float *y, float *z)
{
    if (!hdl || !x || !y || !z) return ESP_ERR_INVALID_ARG;

    uint8_t raw[6];
    if (reg_read8(hdl->i2c_dev, QMI8658_REG_GYR_X_L, raw, 6) != ESP_OK) return -1;

    int16_t gx = (int16_t)(raw[0] | (raw[1] << 8));
    int16_t gy = (int16_t)(raw[2] | (raw[3] << 8));
    int16_t gz = (int16_t)(raw[4] | (raw[5] << 8));

    *x = gx * QMI8658_GYR_SENS_2048;
    *y = gy * QMI8658_GYR_SENS_2048;
    *z = gz * QMI8658_GYR_SENS_2048;
    return ESP_OK;
}

ESP_BOARD_ENTRY_IMPLEMENT(qmi8658, qmi8658_init, qmi8658_deinit);

/* ===================================================================
 *  LTR-308ALS-01 — Ambient Light Sensor
 *  User gave 0x53 (8-bit read addr).  7-bit = 0x53 >> 1 = 0x29.
 * =================================================================== */

struct ltr308als_handle {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t                 gain_idx;
};

#define LTR_REG_CTRL      0x00
#define LTR_REG_MEAS_RATE 0x04
#define LTR_REG_GAIN      0x05
#define LTR_REG_STATUS    0x07
#define LTR_REG_ALS_DATA0 0x0D

#define LTR_CTRL_ACTIVE   0x02
#define LTR_CTRL_RESET    0x10
#define LTR_MEAS_100MS    0x22
#define LTR_STATUS_DRDY   0x08

static const float ltr_gains[] = {
    1.0f, 3.0f, 6.0f, 9.0f, 18.0f,
};

int ltr308als_init(void *cfg, int cfg_size, void **device_handle)
{
    ESP_LOGI(TAG, "LTR-308ALS: init (7-bit addr 0x%02X, user 0x53)",
             LTR308ALS_I2C_ADDR_7BIT);

    i2c_master_bus_handle_t bus;
    if (sensor_get_bus(&bus) != ESP_OK) return -1;

    i2c_master_dev_handle_t dev;
    if (sensor_add_dev(bus, LTR308ALS_I2C_ADDR_7BIT, &dev) != ESP_OK) return -1;

    /* Verify Part ID */
    uint8_t pid = 0;
    reg_read8(dev, LTR308ALS_REG_PART_ID, &pid, 1);
    ESP_LOGI(TAG, "LTR-308ALS: Part ID = 0x%02X (expect 0x%02X)", pid, LTR308ALS_PART_ID_VAL);

    /* Reset, then configure */
    reg_write8(dev, LTR_REG_CTRL, LTR_CTRL_RESET);
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t gain_idx = 1;   /* 3x */
    reg_write8(dev, LTR_REG_GAIN, gain_idx);
    reg_write8(dev, LTR_REG_MEAS_RATE, LTR_MEAS_100MS);
    reg_write8(dev, LTR_REG_CTRL, LTR_CTRL_ACTIVE);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Read back for diagnostics */
    uint8_t ctrl = 0, gain = 0, rate = 0;
    reg_read8(dev, LTR_REG_CTRL, &ctrl, 1);
    reg_read8(dev, LTR_REG_GAIN, &gain, 1);
    reg_read8(dev, LTR_REG_MEAS_RATE, &rate, 1);
    ESP_LOGI(TAG, "LTR-308ALS: register readback -> CTRL: 0x%02X (expect 0x02), GAIN: 0x%02X (expect 0x01), RATE: 0x%02X (expect 0x22)", ctrl, gain, rate);

    ltr308als_handle_t *hdl = calloc(1, sizeof(*hdl));
    if (!hdl) return -1;
    hdl->i2c_dev  = dev;
    hdl->gain_idx = gain_idx;
    *device_handle = hdl;

    ESP_LOGI(TAG, "LTR-308ALS: ready (gain x%.0f)", ltr_gains[gain_idx]);
    return 0;
}

int ltr308als_deinit(void *device_handle)
{
    if (!device_handle) return 0;
    ltr308als_handle_t *hdl = (ltr308als_handle_t *)device_handle;
    reg_write8(hdl->i2c_dev, LTR_REG_CTRL, 0);   /* standby */
    i2c_master_bus_rm_device(hdl->i2c_dev);
    free(hdl);
    ESP_LOGI(TAG, "LTR-308ALS: deinit");
    return 0;
}

esp_err_t ltr308als_read_lux(ltr308als_handle_t *hdl, float *lux)
{
    if (!hdl || !lux) return ESP_ERR_INVALID_ARG;

    uint8_t status = 0;
    if (reg_read8(hdl->i2c_dev, LTR_REG_STATUS, &status, 1) != ESP_OK) return -1;
    if (!(status & LTR_STATUS_DRDY)) {
        return ESP_ERR_NOT_FINISHED;    /* caller retries */
    }

    uint8_t raw[3];
    if (reg_read8(hdl->i2c_dev, LTR_REG_ALS_DATA0, raw, 3) != ESP_OK) return -1;

    uint32_t als_raw = raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16);
    *lux = als_raw * 0.6f / ltr_gains[hdl->gain_idx];
    return ESP_OK;
}

ESP_BOARD_ENTRY_IMPLEMENT(ltr308als, ltr308als_init, ltr308als_deinit);
