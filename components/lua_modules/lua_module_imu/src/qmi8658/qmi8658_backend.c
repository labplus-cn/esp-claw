/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QMI8658 backend for lua_module_imu.
 *
 * Uses the Waveshare QMI8658 component (waveshare/qmi8658) which expects
 * the new ESP-IDF i2c_master_bus_handle_t. We bridge from the framework's
 * i2c_bus_handle_t via i2c_bus_get_internal_bus_handle().
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658.h"

#include "lua_module_imu_backend.h"

static const char *TAG = "lua_module_imu.qmi8658";

/* QMI8658 7-bit I2C addresses: SDO pin selects address. */
#define QMI8658_I2C_ADDR_SDO_LOW   0x6A
#define QMI8658_I2C_ADDR_SDO_HIGH  0x6B

/* Scale factors for float→int conversion. */
#define ACCEL_SCALE_FACTOR  1000  /* m/s² → mm/s² */
#define GYRO_SCALE_FACTOR   1000  /* rad/s → mrad/s */
#define TEMP_SCALE_FACTOR   100   /* °C → centi-degrees */

typedef struct {
    qmi8658_dev_t dev;
    bool initialized;
    bool accel_only;
} qmi8658_state_t;

static esp_err_t qmi8658_backend_probe(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    if (i2c_addr != QMI8658_I2C_ADDR_SDO_LOW && i2c_addr != QMI8658_I2C_ADDR_SDO_HIGH) {
        ESP_LOGE(TAG, "Unsupported QMI8658 address 0x%02x (expected 0x%02x or 0x%02x)",
                 i2c_addr, QMI8658_I2C_ADDR_SDO_LOW, QMI8658_I2C_ADDR_SDO_HIGH);
        return ESP_ERR_INVALID_ARG;
    }

    qmi8658_state_t *st = (qmi8658_state_t *)ctx->state;

    /* Bridge from the legacy i2c_bus handle to the new i2c_master_bus_handle_t
     * that the Waveshare QMI8658 driver expects. */
    i2c_master_bus_handle_t master_bus = i2c_bus_get_internal_bus_handle(ctx->i2c_bus_handle);
    if (master_bus == NULL) {
        ESP_LOGE(TAG, "Failed to get internal i2c_master_bus_handle");
        return ESP_FAIL;
    }

    esp_err_t ret = qmi8658_init(&st->dev, master_bus, i2c_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "qmi8658_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* qmi8658_init() already configures ranges/ODR and enables sensors.
     * Override ODR to 250 Hz (init defaults to 1000 Hz). */
    qmi8658_set_accel_odr(&st->dev, QMI8658_ACCEL_ODR_250HZ);
    qmi8658_set_gyro_odr(&st->dev, QMI8658_GYRO_ODR_250HZ);

    /* Use SI units for accel (m/s²) and gyro (rad/s). */
    qmi8658_set_accel_unit_mps2(&st->dev, true);
    qmi8658_set_gyro_unit_rads(&st->dev, true);
    qmi8658_set_display_precision(&st->dev, 4);

    ESP_RETURN_ON_ERROR(
        qmi8658_enable_sensors(&st->dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO),
        TAG, "Failed to enable QMI8658 sensors");

    /* Sensor needs ~100 ms after enable before producing valid data */
    vTaskDelay(pdMS_TO_TICKS(100));

    st->initialized = true;
    ctx->i2c_addr = i2c_addr;
    ESP_LOGI(TAG, "QMI8658 initialized at addr 0x%02x", i2c_addr);
    return ESP_OK;
}

static void qmi8658_backend_destroy(lua_imu_backend_ctx_t *ctx)
{
    qmi8658_state_t *st = (qmi8658_state_t *)ctx->state;
    if (st->initialized) {
        /* The QMI8658 component does not export a dedicated deinit;
         * just mark as uninitialized. The bus handle lifetime is managed
         * by the framework. */
        st->initialized = false;
    }
}

static esp_err_t qmi8658_backend_read_sample(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out)
{
    qmi8658_state_t *st = (qmi8658_state_t *)ctx->state;
    bool ready = false;

    esp_err_t ret = qmi8658_is_data_ready(&st->dev, &ready);
    if (ret != ESP_OK) {
        return ESP_FAIL;
    }
    if (!ready) {
        return ESP_ERR_NOT_FINISHED;
    }

    if (st->accel_only) {
        float x = 0;
        float y = 0;
        float z = 0;
        ret = qmi8658_read_accel(&st->dev, &x, &y, &z);
        if (ret != ESP_OK) {
            return ESP_FAIL;
        }
        out->accel.x = (int)(x * ACCEL_SCALE_FACTOR);
        out->accel.y = (int)(y * ACCEL_SCALE_FACTOR);
        out->accel.z = (int)(z * ACCEL_SCALE_FACTOR);
        out->sens_time = esp_timer_get_time();
    } else {
        qmi8658_data_t data = { 0 };
        ret = qmi8658_read_sensor_data(&st->dev, &data);
        if (ret != ESP_OK) {
            return ESP_FAIL;
        }

        /* Convert float SI values to scaled integers for the Lua layer. */
        out->accel.x = (int)(data.accelX * ACCEL_SCALE_FACTOR);
        out->accel.y = (int)(data.accelY * ACCEL_SCALE_FACTOR);
        out->accel.z = (int)(data.accelZ * ACCEL_SCALE_FACTOR);
        out->gyro.x = (int)(data.gyroX * GYRO_SCALE_FACTOR);
        out->gyro.y = (int)(data.gyroY * GYRO_SCALE_FACTOR);
        out->gyro.z = (int)(data.gyroZ * GYRO_SCALE_FACTOR);
        out->sens_time = (int64_t)data.timestamp;
    }
    out->status = 0;
    return ESP_OK;
}

static esp_err_t qmi8658_backend_set_accel_only(lua_imu_backend_ctx_t *ctx, bool enabled)
{
    qmi8658_state_t *st = (qmi8658_state_t *)ctx->state;
    uint8_t enable_val = QMI8658_ENABLE_ACCEL;
    if (!enabled) {
        enable_val |= QMI8658_ENABLE_GYRO;
    }
    ESP_RETURN_ON_ERROR(qmi8658_enable_sensors(&st->dev, enable_val),
                        TAG, "Failed to configure QMI8658 sensor mode");
    st->accel_only = enabled;
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_temperature(lua_imu_backend_ctx_t *ctx, int32_t *out)
{
    qmi8658_state_t *st = (qmi8658_state_t *)ctx->state;
    qmi8658_data_t data = { 0 };

    esp_err_t ret = qmi8658_read_sensor_data(&st->dev, &data);
    if (ret != ESP_OK) {
        return ESP_FAIL;
    }
    /* Return temperature in centi-degrees Celsius. */
    *out = (int32_t)(data.temperature * TEMP_SCALE_FACTOR);
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_int_status(lua_imu_backend_ctx_t *ctx, uint32_t *out)
{
    (void)ctx;
    /* QMI8658 does not expose a dedicated interrupt status register
     * through this driver API. Report data-ready as the status. */
    bool ready = false;
    qmi8658_state_t *st = (qmi8658_state_t *)ctx->state;
    esp_err_t ret = qmi8658_is_data_ready(&st->dev, &ready);
    if (ret != ESP_OK) {
        *out = 0;
        return ESP_FAIL;
    }
    *out = ready ? 1 : 0;
    return ESP_OK;
}

static bool qmi8658_backend_is_supported_addr(uint8_t i2c_addr)
{
    return i2c_addr == QMI8658_I2C_ADDR_SDO_LOW || i2c_addr == QMI8658_I2C_ADDR_SDO_HIGH;
}

static uint8_t qmi8658_backend_default_addr(void)
{
    return QMI8658_I2C_ADDR_SDO_LOW;
}

static int qmi8658_backend_sdo_level_for_addr(uint8_t i2c_addr)
{
    return (i2c_addr == QMI8658_I2C_ADDR_SDO_HIGH) ? 1 : 0;
}

const lua_imu_backend_t lua_imu_backend = {
    .chip_name = "qmi8658",
    .state_size = sizeof(qmi8658_state_t),
    .probe = qmi8658_backend_probe,
    .destroy = qmi8658_backend_destroy,
    .read_sample = qmi8658_backend_read_sample,
    .set_accel_only = qmi8658_backend_set_accel_only,
    .read_temperature = qmi8658_backend_read_temperature,
    .read_int_status = qmi8658_backend_read_int_status,
    .is_supported_addr = qmi8658_backend_is_supported_addr,
    .default_addr = qmi8658_backend_default_addr,
    .sdo_level_for_addr = qmi8658_backend_sdo_level_for_addr,
};
