/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "hw_test_cli.h"
#include "esp_board_manager.h"
#include "dev_lcd_touch.h"
#include "dev_display_lcd.h"
#include "esp_lcd_touch.h"
#include "display_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hw_test_touch";

/* ------------------------------------------------------------------ */
/*  Shared state between CLI and touch test task                       */
/* ------------------------------------------------------------------ */

static volatile bool s_touch_running;
static TaskHandle_t  s_touch_task;

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define CROSSHAIR_ARM     12   /* half-length of each crosshair arm (pixels) */
#define CROSSHAIR_THICK    2   /* line thickness (pixels) */
#define TOUCH_POLL_MS     20   /* poll interval (ms) */
#define TOUCH_TASK_STACK  (4 * 1024)

/* Palette of distinct RGB565 colors — cycled for each new touch point */
static const uint16_t s_colors[] = {
    0xFFFF, /* white   */
    0xF800, /* red     */
    0x07E0, /* green   */
    0x001F, /* blue    */
    0xFFE0, /* yellow  */
    0xF81F, /* magenta */
    0x07FF, /* cyan    */
    0xFC00, /* orange  */
};
#define COLOR_COUNT  (sizeof(s_colors) / sizeof(s_colors[0]))

/* ------------------------------------------------------------------ */
/*  Drawing helpers                                                    */
/* ------------------------------------------------------------------ */

/* Draw a crosshair centred at (cx, cy) into the RGB565 frame buffer */
static void draw_crosshair(uint16_t *frame, uint16_t width, uint16_t height,
                           uint16_t cx, uint16_t cy, uint16_t color)
{
    int half = CROSSHAIR_ARM;
    int thick = CROSSHAIR_THICK;

    /* Horizontal bar */
    for (int dy = -thick / 2; dy <= thick / 2; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= height) continue;
        for (int dx = -half; dx <= half; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= width) continue;
            frame[y * width + x] = color;
        }
    }
    /* Vertical bar */
    for (int dx = -thick / 2; dx <= thick / 2; dx++) {
        int x = cx + dx;
        if (x < 0 || x >= width) continue;
        for (int dy = -half; dy <= half; dy++) {
            int y = cy + dy;
            if (y < 0 || y >= height) continue;
            frame[y * width + x] = color;
        }
    }
}

/* Draw a small dot (radius r) to mark the exact touch centre */
static void draw_dot(uint16_t *frame, uint16_t width, uint16_t height,
                     uint16_t cx, uint16_t cy, uint16_t color, int r)
{
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= height) continue;
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int x = cx + dx;
            if (x < 0 || x >= width) continue;
            frame[y * width + x] = color;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Touch test task — runs in its own FreeRTOS task                    */
/* ------------------------------------------------------------------ */

typedef struct {
    esp_lcd_touch_handle_t tp;
    uint16_t lcd_w;
    uint16_t lcd_h;
    uint16_t *frame;
    display_service_session_handle_t session;
} touch_ctx_t;

static void touch_test_task(void *arg)
{
    touch_ctx_t *ctx = (touch_ctx_t *)arg;
    int touch_count = 0;

    ESP_LOGI(TAG, "Touch test task started (%ux%u)", ctx->lcd_w, ctx->lcd_h);

    while (s_touch_running) {
        esp_lcd_touch_read_data(ctx->tp);

        uint16_t x[1] = {0};
        uint16_t y[1] = {0};
        uint16_t strength[1] = {0};
        uint8_t point_num = 0;

        bool touched = esp_lcd_touch_get_coordinates(ctx->tp, x, y, strength,
                                                      &point_num, 1);
        if (!touched || point_num == 0) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }

        touch_count++;
        uint16_t cx = x[0];
        uint16_t cy = y[0];
        uint16_t color = s_colors[(touch_count - 1) % COLOR_COUNT];

        /* Draw crosshair + centre dot (keep all previous marks) */
        draw_crosshair(ctx->frame, ctx->lcd_w, ctx->lcd_h, cx, cy, color);
        draw_dot(ctx->frame, ctx->lcd_w, ctx->lcd_h, cx, cy, color, 2);

        /* Blit full frame to panel */
        display_service_raw_blit_t blit = {
            .x_start = 0, .y_start = 0,
            .x_end = ctx->lcd_w, .y_end = ctx->lcd_h,
            .frame_buffer = ctx->frame,
            .wait = true,
        };
        display_service_session_raw_blit(ctx->session, &blit);

        ESP_LOGI(TAG, "Touch #%d: (%u, %u) strength=%u",
                 touch_count, cx, cy, strength[0]);

        /* Brief debounce: skip same-point duplicates */
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ---- Cleanup ---- */
    ESP_LOGI(TAG, "Touch test done. %d touch(es) detected.", touch_count);
    printf("Touch: %d point(s) tested. Display restoring...\n", touch_count);

    vTaskDelay(pdMS_TO_TICKS(1500));

    free(ctx->frame);
    display_service_close(ctx->session);

    s_touch_running = false;
    s_touch_task = NULL;
    free(ctx);
    ESP_LOGI(TAG, "Display restored.");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  test touch — CLI command handler                                   */
/* ------------------------------------------------------------------ */

int test_touch(int argc, char **argv)
{
    /* ---- stop ---- */
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        if (!s_touch_running) {
            printf("Touch test is not running.\n");
            return 0;
        }
        s_touch_running = false;
        printf("Touch test stop requested.\n");
        return 0;
    }

    /* ---- start ---- */
    if (s_touch_running) {
        printf("Touch test already running. Use 'test touch stop'.\n");
        return 0;
    }

    /* Get touch handle */
    void *touch_dev = NULL;
    esp_err_t err = esp_board_manager_get_device_handle("lcd_touch", &touch_dev);
    if (err != ESP_OK || !touch_dev) {
        printf("Touch device not available: %s\n", esp_err_to_name(err));
        return 0;
    }
    dev_lcd_touch_handles_t *touch_handles = (dev_lcd_touch_handles_t *)touch_dev;
    esp_lcd_touch_handle_t tp = touch_handles->touch_handle;
    if (!tp) {
        printf("Touch handle is NULL\n");
        return 0;
    }

    /* Get LCD dimensions */
    void *dev_cfg = NULL;
    err = esp_board_manager_get_device_config("display_lcd", &dev_cfg);
    if (err != ESP_OK || !dev_cfg) {
        printf("LCD config not found: %s\n", esp_err_to_name(err));
        return 0;
    }
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)dev_cfg;
    uint16_t width = lcd_cfg->lcd_width;
    uint16_t height = lcd_cfg->lcd_height;

    /* Open EXCLUSIVE_RAW display session */
    display_service_session_handle_t session = NULL;
    display_service_session_config_t sess_cfg = {
        .owner_name = "hw_test_touch",
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

    /* Allocate full-frame RGB565 buffer */
    size_t frame_size = (size_t)width * height * sizeof(uint16_t);
    uint16_t *frame = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM);
    if (!frame) {
        printf("Frame buffer alloc failed (%zu bytes)\n", frame_size);
        display_service_close(session);
        return 0;
    }

    /* Clear to black and blit initial frame */
    memset(frame, 0, frame_size);
    display_service_raw_blit_t init_blit = {
        .x_start = 0, .y_start = 0,
        .x_end = width, .y_end = height,
        .frame_buffer = frame,
        .wait = true,
    };
    display_service_session_raw_blit(session, &init_blit);

    /* Build context and spawn task */
    touch_ctx_t *ctx = calloc(1, sizeof(touch_ctx_t));
    if (!ctx) {
        printf("Failed to allocate touch context\n");
        free(frame);
        display_service_close(session);
        return 0;
    }
    ctx->tp = tp;
    ctx->lcd_w = width;
    ctx->lcd_h = height;
    ctx->frame = frame;
    ctx->session = session;

    s_touch_running = true;
    BaseType_t xret = xTaskCreate(touch_test_task, "touch_test", TOUCH_TASK_STACK,
                                  ctx, 5, &s_touch_task);
    if (xret != pdPASS) {
        printf("Failed to create touch test task\n");
        s_touch_running = false;
        free(ctx);
        free(frame);
        display_service_close(session);
        return 0;
    }

    printf("Touch test running (%ux%u). Touch the screen to mark points.\n", width, height);
    printf("Type 'test touch stop' to end.\n");
    return 0;
}
