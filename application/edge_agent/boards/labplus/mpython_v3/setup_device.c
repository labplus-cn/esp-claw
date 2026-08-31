/*
 * SPDX-FileCopyrightText: 2026 Labplus
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_board_device.h"
#include "esp_lcd_jd9853.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "mpython_v3";

/* Board Manager's display_lcd device uses this factory for custom panels. */
esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    if (io == NULL || panel_dev_config == NULL || ret_panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {0};
    memcpy(&panel_cfg, panel_dev_config, sizeof(panel_cfg));

    esp_err_t ret = esp_lcd_new_panel_jd9853(io, &panel_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JD9853 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    /* The 320x172 panel uses a 34-pixel vertical controller offset. */
    ret = esp_lcd_panel_set_gap(*ret_panel, 0, 34);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set JD9853 panel gap: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(*ret_panel);
        *ret_panel = NULL;
    }
    return ret;
}
