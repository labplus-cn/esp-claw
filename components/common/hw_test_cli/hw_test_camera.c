/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "esp_log.h"
#include "esp_board_manager.h"
#include "devices/dev_display_lcd/dev_display_lcd.h"
#include "esp_heap_caps.h"
#include "display_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hw_test_camera";

/* ------------------------------------------------------------------ */
/*  Shared state between CLI and preview task                          */
/* ------------------------------------------------------------------ */

static volatile bool s_preview_running;
static TaskHandle_t  s_preview_task;

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/* Use the CSI capture device (/dev/video0). The esp_video framework
 * routes through the ISP internally when a YUV/RGB format is requested. */
#define CAMERA_DEV_PATH         "/dev/video0"
#define PREVIEW_BUF_COUNT     3
#define PREVIEW_TASK_STACK    (8 * 1024)
/* Request a moderate resolution from the ISP; it will scale internally. */
#define PREVIEW_WIDTH         480
#define PREVIEW_HEIGHT        270

typedef struct {
    void  *start;
    size_t length;
} v4l2_buf_info_t;

/* ------------------------------------------------------------------ */
/*  Preview task — runs in its own FreeRTOS task                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int      fd;
    uint32_t cap_w;
    uint32_t cap_h;
    uint32_t fourcc;      /* V4L2 pixel format FOURCC */
    uint16_t lcd_w;
    uint16_t lcd_h;
    v4l2_buf_info_t bufs[PREVIEW_BUF_COUNT];
    uint32_t buf_count;
    uint16_t *frame_buf;  /* Full LCD frame buffer in PSRAM */
    /* Pre-computed crop parameters (avoid per-frame recalculation) */
    uint32_t crop_w, crop_h, crop_x0, crop_y0;
    display_service_session_handle_t session;
} preview_ctx_t;

static void preview_task(void *arg)
{
    preview_ctx_t *ctx = (preview_ctx_t *)arg;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int skip_frames = 3;

    ESP_LOGI(TAG, "Preview task started (%" PRIu32 "x%" PRIu32 " -> %ux%u LCD)",
             ctx->cap_w, ctx->cap_h, ctx->lcd_w, ctx->lcd_h);

    while (s_preview_running) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "VIDIOC_DQBUF failed (errno=%d)", errno);
            break;
        }

        if (skip_frames > 0) {
            skip_frames--;
            ioctl(ctx->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        const uint8_t *raw = (const uint8_t *)ctx->bufs[buf.index].start;

        if (ctx->fourcc == V4L2_PIX_FMT_RGB565) {
            /* RGB565: center-crop + scale into full frame buffer, then single blit */
            const uint16_t *frame = (const uint16_t *)raw;
            uint16_t *dst = ctx->frame_buf;

            for (uint32_t row = 0; row < ctx->lcd_h; row++) {
                uint32_t src_row = ctx->crop_y0 + row * ctx->crop_h / ctx->lcd_h;
                if (src_row >= ctx->cap_h) src_row = ctx->cap_h - 1;
                const uint16_t *src_line = frame + src_row * ctx->cap_w;
                uint16_t *dst_row = dst + row * ctx->lcd_w;

                for (uint32_t dx = 0; dx < ctx->lcd_w; dx++) {
                    uint32_t sx = ctx->crop_x0 + dx * ctx->crop_w / ctx->lcd_w;
                    if (sx >= ctx->cap_w) sx = ctx->cap_w - 1;
                    dst_row[dx] = src_line[sx];
                }
            }

            display_service_raw_blit_t blit = {
                .x_start = 0, .y_start = 0,
                .x_end = ctx->lcd_w, .y_end = ctx->lcd_h,
                .frame_buffer = dst, .wait = true,
            };
            display_service_session_raw_blit(ctx->session, &blit);
        } else {
            /* Raw/grayscale fallback */
            uint32_t x_step = (ctx->cap_w > ctx->lcd_w) ? (ctx->cap_w / ctx->lcd_w) : 1;
            uint32_t y_step = (ctx->cap_h > ctx->lcd_h) ? (ctx->cap_h / ctx->lcd_h) : 1;
            uint16_t *dst = ctx->frame_buf;

            for (uint32_t row = 0; row < ctx->lcd_h; row++) {
                uint32_t src_row = row * y_step;
                if (src_row >= ctx->cap_h) src_row = ctx->cap_h - 1;
                const uint8_t *src_line = raw + src_row * ctx->cap_w;
                uint16_t *dst_row = dst + row * ctx->lcd_w;

                for (uint32_t dx = 0; dx < ctx->lcd_w; dx++) {
                    uint32_t sx = dx * x_step;
                    if (sx >= ctx->cap_w) sx = ctx->cap_w - 1;
                    uint8_t v = src_line[sx];
                    dst_row[dx] = ((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3);
                }
            }

            display_service_raw_blit_t blit = {
                .x_start = 0, .y_start = 0,
                .x_end = ctx->lcd_w, .y_end = ctx->lcd_h,
                .frame_buffer = dst, .wait = true,
            };
            display_service_session_raw_blit(ctx->session, &blit);
        }

        ioctl(ctx->fd, VIDIOC_QBUF, &buf);
    }

    /* ---- cleanup ---- */
    ESP_LOGI(TAG, "Stopping camera preview...");
    ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    display_service_close(ctx->session);
    free(ctx->frame_buf);

    for (uint32_t i = 0; i < ctx->buf_count; i++) {
        if (ctx->bufs[i].start && ctx->bufs[i].start != MAP_FAILED) {
            munmap(ctx->bufs[i].start, ctx->bufs[i].length);
        }
    }
    close(ctx->fd);

    s_preview_running = false;
    s_preview_task = NULL;
    free(ctx);
    ESP_LOGI(TAG, "Camera preview stopped.");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  test camera — CLI command handler                                  */
/* ------------------------------------------------------------------ */

int test_camera(int argc, char **argv)
{
    /* ---- stop ---- */
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        if (!s_preview_running) {
            printf("Camera preview is not running.\n");
            return 0;
        }
        s_preview_running = false;
        printf("Camera preview stop requested.\n");
        return 0;
    }

    /* ---- start ---- */
    if (s_preview_running) {
        printf("Camera preview already running. Use 'test camera stop'.\n");
        return 0;
    }

    /* Get LCD dimensions */
    void *dev_cfg = NULL;
    esp_err_t err = esp_board_manager_get_device_config("display_lcd", &dev_cfg);
    if (err != ESP_OK || !dev_cfg) {
        printf("LCD device config not found\n");
        return 0;
    }
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)dev_cfg;
    uint16_t lcd_w = lcd_cfg->lcd_width;
    uint16_t lcd_h = lcd_cfg->lcd_height;

    /* Open CSI capture device */
    int fd = open(CAMERA_DEV_PATH, O_RDWR);
    if (fd < 0) {
        printf("Failed to open %s (errno=%d)\n", CAMERA_DEV_PATH, errno);
        return 0;
    }

    /* Query current format first */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        printf("VIDIOC_G_FMT failed (errno=%d)\n", errno);
        close(fd);
        return 0;
    }
    printf("Camera default format: %" PRIu32 "x%" PRIu32 " fourcc=0x%" PRIx32 "\n",
           (uint32_t)fmt.fmt.pix.width, (uint32_t)fmt.fmt.pix.height, (uint32_t)fmt.fmt.pix.pixelformat);

    /* Try to request a smaller resolution (driver may not support it) */
    fmt.fmt.pix.width = PREVIEW_WIDTH;
    fmt.fmt.pix.height = PREVIEW_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        ESP_LOGW(TAG, "VIDIOC_S_FMT %dx%d RGB565 failed (errno=%d), using default",
                 PREVIEW_WIDTH, PREVIEW_HEIGHT, errno);
        /* Re-query to get the driver's current format */
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
            printf("VIDIOC_G_FMT fallback failed (errno=%d)\n", errno);
            close(fd);
            return 0;
        }
    }
    uint32_t cap_w = fmt.fmt.pix.width;
    uint32_t cap_h = fmt.fmt.pix.height;
    uint32_t fourcc = fmt.fmt.pix.pixelformat;
    printf("Camera format: %" PRIu32 "x%" PRIu32 " fourcc=0x%" PRIx32 " size=%" PRIu32 "\n",
           cap_w, cap_h, fourcc, fmt.fmt.pix.sizeimage);

    /* Request buffers */
    struct v4l2_requestbuffers req = {0};
    req.count = PREVIEW_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        printf("VIDIOC_REQBUFS failed (errno=%d)\n", errno);
        close(fd);
        return 0;
    }

    /* mmap buffers */
    v4l2_buf_info_t bufs[PREVIEW_BUF_COUNT];
    memset(bufs, 0, sizeof(bufs));
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            printf("VIDIOC_QUERYBUF[%" PRIu32 "] failed\n", i);
            goto cleanup_close;
        }
        bufs[i].length = buf.length;
        bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, buf.m.offset);
        if (bufs[i].start == MAP_FAILED) {
            printf("mmap[%" PRIu32 "] failed\n", i);
            goto cleanup_unmap;
        }
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            printf("VIDIOC_QBUF[%" PRIu32 "] failed\n", i);
            goto cleanup_unmap;
        }
    }

    /* Start streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("VIDIOC_STREAMON failed (errno=%d)\n", errno);
        goto cleanup_unmap;
    }

    /* Open EXCLUSIVE_RAW display session */
    display_service_session_handle_t session = NULL;
    display_service_session_config_t sess_cfg = {
        .owner_name = "hw_test_camera",
        .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
        .flags = 0,
        .display_config = { .buffer_lines = 16 },
    };
    err = display_service_open(&sess_cfg, &session);
    if (err != ESP_OK) {
        printf("Failed to open display session: %s\n", esp_err_to_name(err));
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        goto cleanup_unmap;
    }

    /* Allocate full-frame RGB565 buffer in PSRAM (lcd_w * lcd_h * 2 bytes) */
    size_t frame_size = (size_t)lcd_w * lcd_h * sizeof(uint16_t);
    uint16_t *frame_buf = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM);
    if (!frame_buf) {
        printf("Failed to allocate frame buffer (%zu bytes)\n", frame_size);
        display_service_close(session);
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        goto cleanup_unmap;
    }

    /* Pre-compute crop parameters (avoid per-frame recalculation) */
    uint32_t crop_w, crop_h, crop_x0, crop_y0;
    if (fourcc == V4L2_PIX_FMT_RGB565) {
        uint32_t lcd_aspect_x1000 = (uint32_t)lcd_w * 1000 / lcd_h;
        uint32_t cam_aspect_x1000 = (uint32_t)cap_w * 1000 / cap_h;
        if (cam_aspect_x1000 > lcd_aspect_x1000) {
            crop_h = cap_h;
            crop_w = (uint32_t)cap_h * lcd_w / lcd_h;
            if (crop_w > cap_w) crop_w = cap_w;
        } else {
            crop_w = cap_w;
            crop_h = (uint32_t)cap_w * lcd_h / lcd_w;
            if (crop_h > cap_h) crop_h = cap_h;
        }
        crop_x0 = (cap_w - crop_w) / 2;
        crop_y0 = (cap_h - crop_h) / 2;
    } else {
        crop_w = cap_w; crop_h = cap_h; crop_x0 = 0; crop_y0 = 0;
    }

    /* Build preview context and spawn task */
    preview_ctx_t *ctx = calloc(1, sizeof(preview_ctx_t));
    if (!ctx) {
        printf("Failed to allocate preview context\n");
        free(frame_buf);
        display_service_close(session);
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        goto cleanup_unmap;
    }
    ctx->fd = fd;
    ctx->cap_w = cap_w;
    ctx->cap_h = cap_h;
    ctx->fourcc = fourcc;
    ctx->lcd_w = lcd_w;
    ctx->lcd_h = lcd_h;
    memcpy(ctx->bufs, bufs, sizeof(bufs));
    ctx->buf_count = req.count;
    ctx->frame_buf = frame_buf;
    ctx->crop_w = crop_w;
    ctx->crop_h = crop_h;
    ctx->crop_x0 = crop_x0;
    ctx->crop_y0 = crop_y0;
    ctx->session = session;

    s_preview_running = true;
    BaseType_t xret = xTaskCreate(preview_task, "cam_preview", PREVIEW_TASK_STACK,
                                  ctx, 5, &s_preview_task);
    if (xret != pdPASS) {
        printf("Failed to create preview task\n");
        s_preview_running = false;
        free(ctx);
        free(frame_buf);
        display_service_close(session);
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        goto cleanup_unmap;
    }

    printf("Camera preview running (%" PRIu32 "x%" PRIu32 " -> %ux%u LCD).\n", cap_w, cap_h, lcd_w, lcd_h);
    printf("Type 'test camera stop' to end.\n");
    return 0;

cleanup_unmap:
    for (uint32_t i = 0; i < req.count; i++) {
        if (bufs[i].start && bufs[i].start != MAP_FAILED) {
            munmap(bufs[i].start, bufs[i].length);
        }
    }
cleanup_close:
    close(fd);
    printf("Camera preview stopped.\n");
    return 0;
}
