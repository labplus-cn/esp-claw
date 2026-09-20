/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MMC5603NJ backend for lua_module_magnetometer.
 * Based on official datasheet Rev.B
 */

#include "sdkconfig.h"

#if CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_MMC5603

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#include "lua_module_mag_backend.h"
#include "mmc5603_regs.h"

static const char *TAG = "lua_mag_mmc5603";

static esp_err_t mmc_write_reg(lua_mag_backend_ctx_t *ctx, uint8_t reg, uint8_t val)
{
    return i2c_bus_write_bytes(ctx->i2c_dev_handle, reg, 1, &val);
}

static esp_err_t mmc_read_reg(lua_mag_backend_ctx_t *ctx, uint8_t reg, uint8_t *val)
{
    return i2c_bus_read_bytes(ctx->i2c_dev_handle, reg, 1, val);
}

static esp_err_t mmc_read_regs(lua_mag_backend_ctx_t *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_bus_read_bytes(ctx->i2c_dev_handle, reg, len, data);
}

static esp_err_t mmc5603_probe(lua_mag_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    ESP_RETURN_ON_ERROR(lua_mag_ctx_select_addr(ctx, i2c_addr), TAG,
                        "Failed to select MMC5603 I2C address 0x%02x", i2c_addr);

    /* Wait for power up */
    vTaskDelay(pdMS_TO_TICKS(MMC5603_POWERUP_DELAY_MS));

    /* Wait for OTP read done */
    uint8_t status = 0;
    int timeout = 50;
    do {
        mmc_read_reg(ctx, MMC5603_REG_STATUS1, &status);
        if (status & MMC5603_STATUS_OTP_DONE) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (--timeout > 0);

    if (!(status & MMC5603_STATUS_OTP_DONE)) {
        ESP_LOGE(TAG, "MMC5603 OTP not ready (status=0x%02X)", status);
        return ESP_ERR_TIMEOUT;
    }

    /* Verify Product ID */
    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(mmc_read_reg(ctx, MMC5603_REG_PRODUCT_ID, &chip_id),
                        TAG, "MMC5603 product ID read failed");
    if (chip_id != MMC5603_PRODUCT_ID_VAL) {
        ESP_LOGE(TAG, "MMC5603 unexpected product ID 0x%02x (expected 0x%02x)",
                 chip_id, MMC5603_PRODUCT_ID_VAL);
        return ESP_ERR_NOT_FOUND;
    }
    ctx->chip_id = chip_id;

    /* Enable Auto SET/RESET */
    ESP_RETURN_ON_ERROR(mmc_write_reg(ctx, MMC5603_REG_CTRL0, MMC5603_CTRL0_AUTO_SR),
                        TAG, "MMC5603 auto SET/RESET config failed");

    ESP_LOGI(TAG, "MMC5603 configured, chip_id=0x%02X", chip_id);
    return ESP_OK;
}

static esp_err_t mmc5603_read_sample(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out)
{
    /* Trigger single measurement */
    ESP_RETURN_ON_ERROR(mmc_write_reg(ctx, MMC5603_REG_CTRL0,
                                      MMC5603_CTRL0_TMM | MMC5603_CTRL0_AUTO_SR),
                        TAG, "MMC5603 trigger measurement failed");

    /* Wait for measurement done */
    uint8_t status = 0;
    int timeout = 200;
    do {
        mmc_read_reg(ctx, MMC5603_REG_STATUS1, &status);
        if (status & MMC5603_STATUS_MEAS_DONE) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (--timeout > 0);

    if (!(status & MMC5603_STATUS_MEAS_DONE)) {
        ESP_LOGW(TAG, "MMC5603 measurement timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* Read 9 bytes of 20-bit data */
    uint8_t data[9] = { 0 };
    ESP_RETURN_ON_ERROR(mmc_read_regs(ctx, MMC5603_REG_XOUT0, data, 9),
                        TAG, "MMC5603 data read failed");

    /* Parse 20-bit UNSIGNED values (MSB first), null-field = 524288 */
    uint32_t raw_x = (uint32_t)((data[0] << 12) | (data[1] << 4) | (data[6] >> 4));
    uint32_t raw_y = (uint32_t)((data[2] << 12) | (data[3] << 4) | (data[7] >> 4));
    uint32_t raw_z = (uint32_t)((data[4] << 12) | (data[5] << 4) | (data[8] >> 4));

    /* Convert to Gauss: (raw - 524288) / 16384 */
    out->x = ((float)raw_x - 524288.0f) / MMC5603_SENSITIVITY;
    out->y = ((float)raw_y - 524288.0f) / MMC5603_SENSITIVITY;
    out->z = ((float)raw_z - 524288.0f) / MMC5603_SENSITIVITY;

    /* Read temperature (optional) */
    uint8_t temp_raw = 0;
    if (mmc_read_reg(ctx, MMC5603_REG_TOUT, &temp_raw) == ESP_OK) {
        /* Temperature = -75 + temp_raw * 0.8 °C */
        out->temperature = -75.0f + (float)temp_raw * 0.8f;
    } else {
        out->temperature = 0.0f;
    }

    out->status = status;
    return ESP_OK;
}

static esp_err_t mmc5603_read_status(lua_mag_backend_ctx_t *ctx, uint8_t *out)
{
    return mmc_read_reg(ctx, MMC5603_REG_STATUS1, out);
}

static bool mmc5603_is_supported_addr(uint8_t a)
{
    return a == MMC5603_I2C_ADDR;
}

static uint8_t mmc5603_default_addr(void)
{
    return MMC5603_I2C_ADDR;
}

const lua_mag_backend_t lua_mag_backend = {
    .chip_name = "mmc5603",
    .state_size = 0,
    .probe = mmc5603_probe,
    .read_sample = mmc5603_read_sample,
    .read_status = mmc5603_read_status,
    .is_supported_addr = mmc5603_is_supported_addr,
    .default_addr = mmc5603_default_addr,
    .probe_alternates = NULL,  /* MMC5603 has single address */
};

#endif /* CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_MMC5603 */
