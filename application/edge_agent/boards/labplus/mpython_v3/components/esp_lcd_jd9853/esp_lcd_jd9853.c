/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/cdefs.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_jd9853.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jd9853";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
    const jd9853_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} jd9853_panel_t;

static esp_err_t panel_jd9853_del(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_init(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_draw_bitmap(esp_lcd_panel_t *panel,
                                           int x_start,
                                           int y_start,
                                           int x_end,
                                           int y_end,
                                           const void *color_data);
static esp_err_t panel_jd9853_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_jd9853_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_jd9853_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_jd9853_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_jd9853_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_jd9853(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    jd9853_panel_t *jd9853 = NULL;

    ESP_GOTO_ON_FALSE(io != NULL && panel_dev_config != NULL && ret_panel != NULL,
                      ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");

    jd9853 = calloc(1, sizeof(*jd9853));
    ESP_GOTO_ON_FALSE(jd9853 != NULL, ESP_ERR_NO_MEM, err, TAG,
                      "no memory for JD9853 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        const gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG,
                          "configure GPIO for reset line failed");
    }

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    switch (panel_dev_config->color_space) {
    case ESP_LCD_COLOR_SPACE_RGB:
        jd9853->madctl_val = 0;
        break;
    case ESP_LCD_COLOR_SPACE_BGR:
        jd9853->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported color space");
    }
#elif ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
    switch (panel_dev_config->rgb_endian) {
    case LCD_RGB_ENDIAN_RGB:
        jd9853->madctl_val = 0;
        break;
    case LCD_RGB_ENDIAN_BGR:
        jd9853->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported RGB endian");
    }
#else
    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        jd9853->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        jd9853->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported RGB element order");
    }
#endif

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        jd9853->colmod_val = 0x55;
        jd9853->fb_bits_per_pixel = 16;
        break;
    case 18:
        jd9853->colmod_val = 0x66;
        jd9853->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported pixel width");
    }

    jd9853->io = io;
    jd9853->reset_gpio_num = panel_dev_config->reset_gpio_num;
    jd9853->reset_level = panel_dev_config->flags.reset_active_high;
    if (panel_dev_config->vendor_config != NULL) {
        const jd9853_vendor_config_t *vendor_config = panel_dev_config->vendor_config;
        jd9853->init_cmds = vendor_config->init_cmds;
        jd9853->init_cmds_size = vendor_config->init_cmds_size;
    }

    jd9853->base.del = panel_jd9853_del;
    jd9853->base.reset = panel_jd9853_reset;
    jd9853->base.init = panel_jd9853_init;
    jd9853->base.draw_bitmap = panel_jd9853_draw_bitmap;
    jd9853->base.invert_color = panel_jd9853_invert_color;
    jd9853->base.set_gap = panel_jd9853_set_gap;
    jd9853->base.mirror = panel_jd9853_mirror;
    jd9853->base.swap_xy = panel_jd9853_swap_xy;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    jd9853->base.disp_off = panel_jd9853_disp_on_off;
#else
    jd9853->base.disp_on_off = panel_jd9853_disp_on_off;
#endif

    *ret_panel = &jd9853->base;
    ESP_LOGD(TAG, "new JD9853 panel @%p", jd9853);
    return ESP_OK;

err:
    if (jd9853 != NULL) {
        if (panel_dev_config != NULL && panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(jd9853);
    }
    return ret;
}

static esp_err_t panel_jd9853_del(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

    if (jd9853->reset_gpio_num >= 0) {
        gpio_reset_pin(jd9853->reset_gpio_num);
    }
    free(jd9853);
    return ESP_OK;
}

static esp_err_t panel_jd9853_reset(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

    if (jd9853->reset_gpio_num >= 0) {
        gpio_set_level(jd9853->reset_gpio_num, jd9853->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(jd9853->reset_gpio_num, !jd9853->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_SWRESET,
                                                       NULL, 0),
                            TAG, "software reset failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t panel_jd9853_init(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_SLPOUT, NULL, 0),
                        TAG, "sleep-out command failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_MADCTL,
                                                   &jd9853->madctl_val, 1),
                        TAG, "MADCTL command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_COLMOD,
                                                   &jd9853->colmod_val, 1),
                        TAG, "COLMOD command failed");

    for (uint16_t i = 0; i < jd9853->init_cmds_size; ++i) {
        const jd9853_lcd_init_cmd_t *cmd = &jd9853->init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, cmd->cmd,
                                                       cmd->data, cmd->data_bytes),
                            TAG, "vendor initialization command failed");
        if (cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t panel_jd9853_draw_bitmap(esp_lcd_panel_t *panel,
                                           int x_start,
                                           int y_start,
                                           int x_end,
                                           int y_end,
                                           const void *color_data)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    assert(x_start < x_end && y_start < y_end);

    x_start += jd9853->x_gap;
    x_end += jd9853->x_gap;
    y_start += jd9853->y_gap;
    y_end += jd9853->y_gap;

    const uint8_t column_data[] = {
        (uint8_t)(x_start >> 8), (uint8_t)x_start,
        (uint8_t)((x_end - 1) >> 8), (uint8_t)(x_end - 1),
    };
    const uint8_t row_data[] = {
        (uint8_t)(y_start >> 8), (uint8_t)y_start,
        (uint8_t)((y_end - 1) >> 8), (uint8_t)(y_end - 1),
    };

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_CASET,
                                                   column_data, sizeof(column_data)),
                        TAG, "column address command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_RASET,
                                                   row_data, sizeof(row_data)),
                        TAG, "row address command failed");

    const size_t length = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) *
                          jd9853->fb_bits_per_pixel / 8;
    return esp_lcd_panel_io_tx_color(jd9853->io, LCD_CMD_RAMWR, color_data, length);
}

static esp_err_t panel_jd9853_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    return esp_lcd_panel_io_tx_param(jd9853->io,
                                     invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF,
                                     NULL, 0);
}

static esp_err_t panel_jd9853_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

    if (mirror_x) {
        jd9853->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        jd9853->madctl_val &= (uint8_t)~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        jd9853->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        jd9853->madctl_val &= (uint8_t)~LCD_CMD_MY_BIT;
    }
    return esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_MADCTL,
                                     &jd9853->madctl_val, 1);
}

static esp_err_t panel_jd9853_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

    if (swap_axes) {
        jd9853->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        jd9853->madctl_val &= (uint8_t)~LCD_CMD_MV_BIT;
    }
    return esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_MADCTL,
                                     &jd9853->madctl_val, 1);
}

static esp_err_t panel_jd9853_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    jd9853->x_gap = x_gap;
    jd9853->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_jd9853_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    on_off = !on_off;
#endif

    return esp_lcd_panel_io_tx_param(jd9853->io,
                                     on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF,
                                     NULL, 0);
}
