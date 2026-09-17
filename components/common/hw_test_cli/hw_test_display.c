/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_board_manager.h"
#include "esp_lcd_panel_ops.h"
#include "devices/dev_display_lcd/dev_display_lcd.h"
#include "display_service.h"
#include "esp_heap_caps.h"

static const char *TAG = "hw_test_display";

/*
 * test screen — sequentially fill the LCD with solid colors (R G B W K),
 * each lasting 5 seconds.
 *
 * Opens an EXCLUSIVE_RAW display session so the blit goes directly to the
 * panel framebuffer without going through LVGL.  The session is released
 * before returning so the UI restores automatically.
 */
int test_screen(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Get LCD dimensions from board config */
    void *dev_cfg = NULL;
    esp_err_t err = esp_board_manager_get_device_config("display_lcd", &dev_cfg);
    if (err != ESP_OK || !dev_cfg) {
        printf("LCD device config not found: %s\n", esp_err_to_name(err));
        return 0;
    }
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)dev_cfg;
    uint16_t width = lcd_cfg->lcd_width;
    uint16_t height = lcd_cfg->lcd_height;

    /* Open an EXCLUSIVE_RAW session to bypass LVGL */
    display_service_session_handle_t session = NULL;
    display_service_session_config_t sess_cfg = {
        .owner_name = "hw_test_screen",
        .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
        .flags = 0,
        .display_config = {
            .buffer_lines = 16,
            .tick_ms = 0,
            .task_period_ms = 0,
        },
    };
    err = display_service_open(&sess_cfg, &session);
    if (err != ESP_OK) {
        printf("Failed to open display session: %s\n", esp_err_to_name(err));
        return 0;
    }

    /* Allocate a full-frame RGB565 buffer */
    size_t frame_size = (size_t)width * height * sizeof(uint16_t);
    uint16_t *frame = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM);
    if (!frame) {
        printf("Failed to allocate frame buffer (%zu bytes)\n", frame_size);
        display_service_close(session);
        return 0;
    }

    /* Colors: Red, Green, Blue, White, Black (RGB565) */
    const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000 };
    const char *names[]     = { "RED",   "GREEN", "BLUE",   "WHITE",  "BLACK" };
    const int color_count = sizeof(colors) / sizeof(colors[0]);

    for (int c = 0; c < color_count; c++) {
        /* Fill entire frame with the current color */
        size_t total_pixels = (size_t)width * height;
        for (size_t i = 0; i < total_pixels; i++) {
            frame[i] = colors[c];
        }

        /* Single blit for the whole screen */
        display_service_raw_blit_t blit = {
            .x_start = 0,
            .y_start = 0,
            .x_end = width,
            .y_end = height,
            .frame_buffer = frame,
            .wait = true,
        };
        display_service_session_raw_blit(session, &blit);
        printf("  [%s] full screen (%ux%u)\n", names[c], width, height);

        /* Hold for 5 seconds before next color */
        if (c < color_count - 1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    free(frame);

    printf("Screen test complete. Display will restore in 3s...\n");
    vTaskDelay(pdMS_TO_TICKS(3000));
    display_service_close(session);
    printf("Display restored.\n");
    return 0;
}
