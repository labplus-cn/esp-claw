/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "hw_test_cli.h"
#include "esp_board_manager.h"
#include "dev_display_lcd.h"
#include "display_service.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hw_test_lvgl";

/* ------------------------------------------------------------------ */
/*  Shared state between CLI and LVGL test task                        */
/* ------------------------------------------------------------------ */

static volatile bool s_lvgl_test_running;
static TaskHandle_t  s_lvgl_test_task;

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define LVGL_TEST_TASK_STACK  (6 * 1024)
#define BTN_COLS  3
#define BTN_ROWS  3
#define BTN_COUNT (BTN_COLS * BTN_ROWS)

/* Button colour palette (raw RGB888 values; converted at runtime) */
static const uint32_t s_btn_colors[BTN_COUNT] = {
    0xE53935, /* red     */
    0x1E88E5, /* blue    */
    0x43A047, /* green   */
    0xFB8C00, /* orange  */
    0x8E24AA, /* purple  */
    0x00ACC1, /* cyan    */
    0xFDD835, /* yellow  */
    0x6D4C41, /* brown   */
    0x546E7A, /* grey    */
};

/* ------------------------------------------------------------------ */
/*  LVGL event context                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    lv_obj_t *count_label;
    int       touch_count;
} lvgl_test_ctx_t;

static void btn_click_cb(lv_event_t *e)
{
    lvgl_test_ctx_t *ctx = (lvgl_test_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    ctx->touch_count++;

    /* Flash the pressed button briefly */
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);

    /* Update counter label */
    char buf[48];
    snprintf(buf, sizeof(buf), "Touches: %d", ctx->touch_count);
    lv_label_set_text(ctx->count_label, buf);

    ESP_LOGI(TAG, "Button clicked (total: %d)", ctx->touch_count);
}

/* Timer to restore button colour after a press flash.
 * user_data layout: pointer with low 4 bits = button index (aligned ptrs are 16-byte aligned). */
static void btn_restore_timer_cb(lv_timer_t *timer)
{
    uintptr_t val = (uintptr_t)lv_timer_get_user_data(timer);
    lv_obj_t *btn = (lv_obj_t *)(val & ~(uintptr_t)0xF);
    int idx = (int)(val & 0xF);
    lv_obj_set_style_bg_color(btn, lv_color_hex(s_btn_colors[idx]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_timer_del(timer);
}

static void btn_pressed_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    /* Button index was passed as user_data when registering the callback */
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    /* Encode index into timer user_data together with the button pointer */
    uintptr_t val = (uintptr_t)btn | (uintptr_t)(idx & 0xF);
    lv_timer_t *tmr = lv_timer_create(btn_restore_timer_cb, 200, (void *)val);
    lv_timer_set_repeat_count(tmr, 1);
}

/* ------------------------------------------------------------------ */
/*  LVGL test task                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t lcd_w;
    uint16_t lcd_h;
    display_service_session_handle_t session;
} lvgl_test_params_t;

static void lvgl_test_task(void *arg)
{
    lvgl_test_params_t *params = (lvgl_test_params_t *)arg;
    lvgl_test_ctx_t ctx = {0};

    ESP_LOGI(TAG, "LVGL test task started (%ux%u)", params->lcd_w, params->lcd_h);

    /* ---- Create screen and widgets (LVGL lock required) ---- */
    display_service_lock();

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_flag(screen, LV_OBJ_FLAG_SCROLLABLE, false);

    /* Title */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "LVGL Touch Test");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Counter label */
    ctx.count_label = lv_label_create(screen);
    lv_label_set_text(ctx.count_label, "Touches: 0");
    lv_obj_set_style_text_color(ctx.count_label, lv_color_hex(0x00E676), LV_PART_MAIN);
    lv_obj_align(ctx.count_label, LV_ALIGN_TOP_MID, 0, 60);

    /* Hint label */
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "Tap buttons to test touch");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* Button grid */
    int pad = 12;
    int top_offset = 100;
    int grid_w = params->lcd_w - pad * 2;
    int grid_h = params->lcd_h - top_offset - 60;
    int btn_w = (grid_w - pad * (BTN_COLS + 1)) / BTN_COLS;
    int btn_h = (grid_h - pad * (BTN_ROWS + 1)) / BTN_ROWS;

    for (int row = 0; row < BTN_ROWS; row++) {
        for (int col = 0; col < BTN_COLS; col++) {
            int idx = row * BTN_COLS + col;
            lv_obj_t *btn = lv_obj_create(screen);
            lv_obj_set_size(btn, btn_w, btn_h);
            int x = pad + col * (btn_w + pad);
            int y = top_offset + pad + row * (btn_h + pad);
            lv_obj_set_pos(btn, x, y);
            lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(s_btn_colors[idx]), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
            lv_obj_set_flag(btn, LV_OBJ_FLAG_SCROLLABLE, false);
            lv_obj_set_flag(btn, LV_OBJ_FLAG_CLICKABLE, true);

            /* Index label inside button */
            lv_obj_t *lbl = lv_label_create(btn);
            char num[4];
            snprintf(num, sizeof(num), "%d", idx + 1);
            lv_label_set_text(lbl, num);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_center(lbl);
            lv_obj_set_flag(lbl, LV_OBJ_FLAG_CLICKABLE, false);

            lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, &ctx);
            lv_obj_add_event_cb(btn, btn_pressed_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)idx);
        }
    }

    /* Load screen */
    display_service_session_load_screen_locked(params->session, screen);
    display_service_unlock();

    ESP_LOGI(TAG, "LVGL test screen loaded");

    /* ---- Wait for stop request ---- */
    while (s_lvgl_test_running) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* ---- Cleanup ---- */
    ESP_LOGI(TAG, "LVGL test stopping, %d touch(es) detected.", ctx.touch_count);
    printf("LVGL test: %d touch(es) detected. Display restoring...\n", ctx.touch_count);

    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Close session — automatically restores default screen via flag */
    display_service_close(params->session);

    s_lvgl_test_running = false;
    s_lvgl_test_task = NULL;
    free(params);
    ESP_LOGI(TAG, "Display restored.");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  test lvgl — CLI command handler                                    */
/* ------------------------------------------------------------------ */

int test_lvgl(int argc, char **argv)
{
    /* ---- stop ---- */
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        if (!s_lvgl_test_running) {
            printf("LVGL test is not running.\n");
            return 0;
        }
        s_lvgl_test_running = false;
        printf("LVGL test stop requested.\n");
        return 0;
    }

    /* ---- start ---- */
    if (s_lvgl_test_running) {
        printf("LVGL test already running. Use 'test lvgl stop'.\n");
        return 0;
    }

    /* Get LCD dimensions */
    void *dev_cfg = NULL;
    esp_err_t err = esp_board_manager_get_device_config("display_lcd", &dev_cfg);
    if (err != ESP_OK || !dev_cfg) {
        printf("LCD config not found: %s\n", esp_err_to_name(err));
        return 0;
    }
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)dev_cfg;
    uint16_t width = lcd_cfg->lcd_width;
    uint16_t height = lcd_cfg->lcd_height;

    /* Open EXCLUSIVE_LVGL session with auto-restore */
    display_service_session_handle_t session = NULL;
    display_service_session_config_t sess_cfg = {
        .owner_name = "hw_test_lvgl",
        .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
        .flags = DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE,
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

    /* Build params and spawn task */
    lvgl_test_params_t *params = calloc(1, sizeof(lvgl_test_params_t));
    if (!params) {
        printf("Failed to allocate test params\n");
        display_service_close(session);
        return 0;
    }
    params->lcd_w = width;
    params->lcd_h = height;
    params->session = session;

    s_lvgl_test_running = true;
    BaseType_t xret = xTaskCreate(lvgl_test_task, "lvgl_test", LVGL_TEST_TASK_STACK,
                                  params, 5, &s_lvgl_test_task);
    if (xret != pdPASS) {
        printf("Failed to create LVGL test task\n");
        s_lvgl_test_running = false;
        free(params);
        display_service_close(session);
        return 0;
    }

    printf("LVGL touch test running (%ux%u). Tap buttons to test.\n", width, height);
    printf("Type 'test lvgl stop' to end.\n");
    return 0;
}
