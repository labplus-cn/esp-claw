/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include "esp_log.h"
#include "esp_lcd_jd9853.h"
#include "dev_display_lcd.h"
#include "esp_board_manager_includes.h"
#include "driver/i2c_master.h"
#include <string.h>

#include "driver/gpio.h"

static const char *TAG = "x_card_board";

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG, "Creating jd9853 panel...");

    i2c_master_bus_handle_t bus_handle = NULL;
    esp_board_manager_get_periph_handle(ESP_BOARD_PERIPH_NAME_I2C_MASTER, (void **)&bus_handle);
    i2c_device_config_t stm8_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 17,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t stm8_dev_handle = NULL;
    i2c_master_bus_add_device(bus_handle, &stm8_dev_cfg, &stm8_dev_handle);
    uint8_t reg = 4;
    i2c_master_transmit(stm8_dev_handle, &reg, 1, 1000 / portTICK_PERIOD_MS);
    i2c_master_bus_rm_device(stm8_dev_handle);

    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    esp_err_t ret = esp_lcd_new_panel_jd9853(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE("lcd_panel_factory_entry_t", "New JD9853 panel failed");
        return ret;
    }

    return ESP_OK;
}
