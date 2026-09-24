/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB UVC camera production test. Frames from /dev/video40 are converted to
 * RGB565 and displayed through the display service's exclusive raw session.
 */

#include "hw_test_usb_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "display_service.h"
#include "devices/dev_display_lcd/dev_display_lcd.h"
#include "esp_board_manager.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#define USB_CAMERA_DEVICE_PATH          ESP_VIDEO_USB_UVC_NAME(0)
#define USB_CAMERA_BUFFER_COUNT         3
#define USB_CAMERA_TASK_STACK_SIZE      (8 * 1024)
#define USB_CAMERA_TASK_PRIORITY        5
#define USB_CAMERA_OPEN_RETRY_MS        100
#define USB_CAMERA_RECONNECT_DELAY_MS   500
#define USB_CAMERA_DQBUF_TIMEOUT_MS     1000
#define USB_CAMERA_STOP_TIMEOUT_MS      5000
#define USB_CAMERA_MAX_DQBUF_ERRORS     5
#define USB_CAMERA_FORMAT_INDEX_LIMIT   8
#define USB_CAMERA_FRAME_SIZE_LIMIT     32
#define USB_CAMERA_TARGET_WIDTH         640
#define USB_CAMERA_TARGET_HEIGHT        480
#define USB_CAMERA_MAX_YUYV_PIXELS      (1280U * 720U)

static const char *TAG = "test_usb_camera";

typedef struct {
    void *start;
    size_t length;
} usb_camera_buffer_t;

typedef struct {
    bool valid;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint64_t score;
} usb_camera_format_t;

typedef struct {
    int fd;
    bool stream_started;
    bool buffers_requested;
    uint32_t pixel_format;
    uint32_t capture_width;
    uint32_t capture_height;
    uint16_t lcd_width;
    uint16_t lcd_height;
    uint16_t draw_x;
    uint16_t draw_y;
    uint16_t draw_width;
    uint16_t draw_height;
    uint32_t *source_x;
    uint32_t *source_y;
    usb_camera_buffer_t buffers[USB_CAMERA_BUFFER_COUNT];
    uint32_t buffer_count;
    uint16_t *lcd_frame;
    uint16_t *jpeg_frame;
    jpeg_dec_handle_t jpeg_decoder;
    display_service_session_handle_t display_session;
    volatile bool stop_requested;
    uint32_t frames_displayed;
    uint32_t frame_errors;
} usb_camera_preview_t;

static portMUX_TYPE s_preview_lock = portMUX_INITIALIZER_UNLOCKED;
static usb_camera_preview_t *s_preview;
static bool s_preview_starting;

static int usb_camera_open_wait(usb_camera_preview_t *preview, const char *path)
{
    int last_errno = ENODEV;

    while (!preview->stop_requested) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            return fd;
        }

        last_errno = errno;
        if (last_errno != ENODEV && last_errno != EBUSY) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(USB_CAMERA_OPEN_RETRY_MS));
    }

    errno = last_errno;
    return -1;
}

static const char *usb_camera_format_name(uint32_t pixel_format)
{
    switch (pixel_format) {
    case V4L2_PIX_FMT_YUYV:
        return "YUYV";
    case V4L2_PIX_FMT_JPEG:
        return "MJPEG";
    default:
        return "unsupported";
    }
}

static uint64_t usb_camera_resolution_score(uint32_t width, uint32_t height)
{
    uint32_t width_delta = width > USB_CAMERA_TARGET_WIDTH ?
                           width - USB_CAMERA_TARGET_WIDTH : USB_CAMERA_TARGET_WIDTH - width;
    uint32_t height_delta = height > USB_CAMERA_TARGET_HEIGHT ?
                            height - USB_CAMERA_TARGET_HEIGHT : USB_CAMERA_TARGET_HEIGHT - height;

    return (uint64_t)width_delta * USB_CAMERA_TARGET_HEIGHT +
           (uint64_t)height_delta * USB_CAMERA_TARGET_WIDTH;
}

static void usb_camera_consider_format(usb_camera_format_t *candidate,
                                       uint32_t pixel_format,
                                       uint32_t width,
                                       uint32_t height)
{
    if (width == 0 || height == 0) {
        return;
    }

    uint64_t score = usb_camera_resolution_score(width, height);
    if (!candidate->valid || score < candidate->score ||
            (score == candidate->score && (uint64_t)width * height <
             (uint64_t)candidate->width * candidate->height)) {
        candidate->valid = true;
        candidate->pixel_format = pixel_format;
        candidate->width = width;
        candidate->height = height;
        candidate->score = score;
    }
}

static esp_err_t usb_camera_select_format(int fd, usb_camera_format_t *selected)
{
    usb_camera_format_t yuyv = {0};
    usb_camera_format_t mjpeg = {0};

    for (uint32_t format_index = 0; format_index < USB_CAMERA_FORMAT_INDEX_LIMIT; format_index++) {
        struct v4l2_fmtdesc format_desc = {
            .index = format_index,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        };
        if (ioctl(fd, VIDIOC_ENUM_FMT, &format_desc) < 0) {
            continue;
        }
        if (format_desc.pixelformat != V4L2_PIX_FMT_YUYV &&
                format_desc.pixelformat != V4L2_PIX_FMT_JPEG) {
            ESP_LOGI(TAG, "Ignoring unsupported UVC format " V4L2_FMT_STR,
                     V4L2_FMT_STR_ARG(format_desc.pixelformat));
            continue;
        }

        usb_camera_format_t *candidate = format_desc.pixelformat == V4L2_PIX_FMT_YUYV ?
                                         &yuyv : &mjpeg;
        for (uint32_t size_index = 0; size_index < USB_CAMERA_FRAME_SIZE_LIMIT; size_index++) {
            struct v4l2_frmsizeenum frame_size = {
                .index = size_index,
                .pixel_format = format_desc.pixelformat,
            };
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) < 0) {
                break;
            }
            if (frame_size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                continue;
            }
            usb_camera_consider_format(candidate, format_desc.pixelformat,
                                       frame_size.discrete.width, frame_size.discrete.height);
        }
    }

    /* Some UVC implementations expose sparse format indices. G_FMT is also a
     * useful fallback if enumeration did not return a usable entry. */
    if (!yuyv.valid && !mjpeg.valid) {
        struct v4l2_format current = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        };
        if (ioctl(fd, VIDIOC_G_FMT, &current) == 0 &&
                (current.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV ||
                 current.fmt.pix.pixelformat == V4L2_PIX_FMT_JPEG)) {
            usb_camera_format_t *candidate = current.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV ?
                                             &yuyv : &mjpeg;
            usb_camera_consider_format(candidate, current.fmt.pix.pixelformat,
                                       current.fmt.pix.width, current.fmt.pix.height);
        }
    }

    if (yuyv.valid && (uint64_t)yuyv.width * yuyv.height <= USB_CAMERA_MAX_YUYV_PIXELS) {
        *selected = yuyv;
    } else if (mjpeg.valid) {
        *selected = mjpeg;
    } else if (yuyv.valid) {
        *selected = yuyv;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static uint8_t usb_camera_clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > UINT8_MAX) {
        return UINT8_MAX;
    }
    return (uint8_t)value;
}

static uint16_t usb_camera_yuv_to_rgb565(uint8_t y, uint8_t u, uint8_t v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;

    if (c < 0) {
        c = 0;
    }
    uint8_t r = usb_camera_clamp_u8((298 * c + 409 * e + 128) >> 8);
    uint8_t g = usb_camera_clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    uint8_t b = usb_camera_clamp_u8((298 * c + 516 * d + 128) >> 8);

    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void usb_camera_scale_yuyv(usb_camera_preview_t *preview, const uint8_t *source)
{
    for (uint32_t dy = 0; dy < preview->draw_height; dy++) {
        uint32_t sy = preview->source_y[dy];
        uint16_t *destination = preview->lcd_frame +
                                (size_t)(preview->draw_y + dy) * preview->lcd_width + preview->draw_x;
        const uint8_t *source_row = source + (size_t)sy * preview->capture_width * 2;

        for (uint32_t dx = 0; dx < preview->draw_width; dx++) {
            uint32_t sx = preview->source_x[dx];
            const uint8_t *pair = source_row + (sx & ~1U) * 2;
            uint8_t y = pair[(sx & 1U) ? 2 : 0];
            destination[dx] = usb_camera_yuv_to_rgb565(y, pair[1], pair[3]);
        }
    }
}

static void usb_camera_scale_rgb565(usb_camera_preview_t *preview, const uint16_t *source)
{
    for (uint32_t dy = 0; dy < preview->draw_height; dy++) {
        const uint16_t *source_row = source +
                                     (size_t)preview->source_y[dy] * preview->capture_width;
        uint16_t *destination = preview->lcd_frame +
                                (size_t)(preview->draw_y + dy) * preview->lcd_width + preview->draw_x;

        for (uint32_t dx = 0; dx < preview->draw_width; dx++) {
            destination[dx] = source_row[preview->source_x[dx]];
        }
    }
}

static esp_err_t usb_camera_decode_mjpeg(usb_camera_preview_t *preview,
                                         const uint8_t *data,
                                         size_t data_size)
{
    if (data_size == 0 || data_size > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_dec_io_t io = {
        .inbuf = (uint8_t *)data,
        .inbuf_len = (int)data_size,
        .outbuf = (uint8_t *)preview->jpeg_frame,
    };
    jpeg_dec_header_info_t header = {0};
    jpeg_error_t jpeg_err = jpeg_dec_parse_header(preview->jpeg_decoder, &io, &header);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "MJPEG header decode failed: %d", jpeg_err);
        return ESP_FAIL;
    }
    if (header.width != preview->capture_width || header.height != preview->capture_height) {
        ESP_LOGW(TAG, "MJPEG frame size changed: %ux%u, expected %" PRIu32 "x%" PRIu32,
                 header.width, header.height, preview->capture_width, preview->capture_height);
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_err = jpeg_dec_process(preview->jpeg_decoder, &io);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "MJPEG frame decode failed: %d", jpeg_err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void usb_camera_release(usb_camera_preview_t *preview)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (preview->fd >= 0 && preview->stream_started) {
        if (ioctl(preview->fd, VIDIOC_STREAMOFF, &type) < 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed (errno=%d)", errno);
        }
        preview->stream_started = false;
    }
    if (preview->display_session) {
        esp_err_t err = display_service_close(preview->display_session);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Display session close failed: %s", esp_err_to_name(err));
        }
        preview->display_session = NULL;
    }
    if (preview->jpeg_decoder) {
        (void)jpeg_dec_close(preview->jpeg_decoder);
        preview->jpeg_decoder = NULL;
    }
    if (preview->jpeg_frame) {
        jpeg_free_align(preview->jpeg_frame);
        preview->jpeg_frame = NULL;
    }
    free(preview->lcd_frame);
    preview->lcd_frame = NULL;
    free(preview->source_x);
    preview->source_x = NULL;
    free(preview->source_y);
    preview->source_y = NULL;

    for (uint32_t i = 0; i < preview->buffer_count; i++) {
        if (preview->buffers[i].start && preview->buffers[i].start != MAP_FAILED) {
            (void)munmap(preview->buffers[i].start, preview->buffers[i].length);
            preview->buffers[i].start = NULL;
            preview->buffers[i].length = 0;
        }
    }
    if (preview->fd >= 0 && preview->buffers_requested) {
        struct v4l2_requestbuffers request = {
            .count = 0,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        (void)ioctl(preview->fd, VIDIOC_REQBUFS, &request);
        preview->buffers_requested = false;
    }
    if (preview->fd >= 0) {
        if (close(preview->fd) < 0) {
            ESP_LOGW(TAG, "Close %s failed (errno=%d)", USB_CAMERA_DEVICE_PATH, errno);
        }
        preview->fd = -1;
    }
    preview->buffer_count = 0;
}

static void usb_camera_preview_task(void *arg);

static esp_err_t usb_camera_prepare_display(usb_camera_preview_t *preview)
{
    void *display_config = NULL;
    esp_err_t err = esp_board_manager_get_device_config("display_lcd", &display_config);
    if (err != ESP_OK || !display_config) {
        ESP_LOGE(TAG, "LCD device configuration is unavailable");
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    dev_display_lcd_config_t *lcd_config = (dev_display_lcd_config_t *)display_config;
    preview->lcd_width = lcd_config->lcd_width;
    preview->lcd_height = lcd_config->lcd_height;
    if (preview->lcd_width == 0 || preview->lcd_height == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t capture_ratio = (uint64_t)preview->capture_width * preview->lcd_height;
    uint64_t display_ratio = (uint64_t)preview->lcd_width * preview->capture_height;
    if (capture_ratio > display_ratio) {
        preview->draw_width = preview->lcd_width;
        preview->draw_height = (uint32_t)preview->capture_height * preview->lcd_width /
                               preview->capture_width;
    } else {
        preview->draw_height = preview->lcd_height;
        preview->draw_width = (uint32_t)preview->capture_width * preview->lcd_height /
                              preview->capture_height;
    }
    if (preview->draw_width == 0 || preview->draw_height == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    preview->draw_x = (preview->lcd_width - preview->draw_width) / 2;
    preview->draw_y = (preview->lcd_height - preview->draw_height) / 2;

    preview->source_x = calloc(preview->draw_width, sizeof(*preview->source_x));
    preview->source_y = calloc(preview->draw_height, sizeof(*preview->source_y));
    if (!preview->source_x || !preview->source_y) {
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t x = 0; x < preview->draw_width; x++) {
        preview->source_x[x] = (uint64_t)x * preview->capture_width / preview->draw_width;
    }
    for (uint32_t y = 0; y < preview->draw_height; y++) {
        preview->source_y[y] = (uint64_t)y * preview->capture_height / preview->draw_height;
    }

    size_t lcd_frame_size = (size_t)preview->lcd_width * preview->lcd_height * sizeof(uint16_t);
    preview->lcd_frame = heap_caps_calloc(1, lcd_frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!preview->lcd_frame) {
        ESP_LOGE(TAG, "Failed to allocate %zu-byte LCD frame buffer", lcd_frame_size);
        return ESP_ERR_NO_MEM;
    }

    display_service_session_config_t session_config = {
        .owner_name = "hw_test_usb_camera",
        .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
        .flags = 0,
        .display_config = {
            .buffer_lines = 16,
        },
    };
    return display_service_open(&session_config, &preview->display_session);
}

static esp_err_t usb_camera_connect(usb_camera_preview_t *preview)
{
    esp_err_t err = ESP_FAIL;
    preview->fd = usb_camera_open_wait(preview, USB_CAMERA_DEVICE_PATH);
    if (preview->fd < 0) {
        if (!preview->stop_requested) {
            ESP_LOGW(TAG, "Failed to open %s (errno=%d)", USB_CAMERA_DEVICE_PATH, errno);
        }
        return ESP_ERR_NOT_FOUND;
    }

    usb_camera_format_t selected = {0};
    err = usb_camera_select_format(preview->fd, &selected);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera has no supported YUYV or MJPEG mode");
        goto fail;
    }

    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = selected.width,
            .height = selected.height,
            .pixelformat = selected.pixel_format,
        },
    };
    if (ioctl(preview->fd, VIDIOC_S_FMT, &format) < 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT %s %" PRIu32 "x%" PRIu32 " failed (errno=%d)",
                 usb_camera_format_name(selected.pixel_format), selected.width, selected.height, errno);
        err = ESP_FAIL;
        goto fail;
    }
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(preview->fd, VIDIOC_G_FMT, &format) < 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed (errno=%d)", errno);
        err = ESP_FAIL;
        goto fail;
    }
    preview->pixel_format = format.fmt.pix.pixelformat;
    preview->capture_width = format.fmt.pix.width;
    preview->capture_height = format.fmt.pix.height;
    if (preview->pixel_format != V4L2_PIX_FMT_YUYV &&
            preview->pixel_format != V4L2_PIX_FMT_JPEG) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }

    struct timeval dequeue_timeout = {
        .tv_sec = USB_CAMERA_DQBUF_TIMEOUT_MS / 1000,
        .tv_usec = (USB_CAMERA_DQBUF_TIMEOUT_MS % 1000) * 1000,
    };
    if (ioctl(preview->fd, VIDIOC_S_DQBUF_TIMEOUT, &dequeue_timeout) < 0) {
        ESP_LOGE(TAG, "Failed to configure frame timeout (errno=%d)", errno);
        err = ESP_FAIL;
        goto fail;
    }

    struct v4l2_requestbuffers request = {
        .count = USB_CAMERA_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(preview->fd, VIDIOC_REQBUFS, &request) < 0 || request.count == 0 ||
            request.count > USB_CAMERA_BUFFER_COUNT) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed (count=%" PRIu32 ", errno=%d)", request.count, errno);
        err = ESP_FAIL;
        goto fail;
    }
    preview->buffers_requested = true;
    preview->buffer_count = request.count;

    for (uint32_t i = 0; i < preview->buffer_count; i++) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(preview->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%" PRIu32 "] failed (errno=%d)", i, errno);
            err = ESP_FAIL;
            goto fail;
        }
        preview->buffers[i].length = buffer.length;
        preview->buffers[i].start = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, preview->fd, buffer.m.offset);
        if (preview->buffers[i].start == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%" PRIu32 "] failed (errno=%d)", i, errno);
            err = ESP_FAIL;
            goto fail;
        }
        if (ioctl(preview->fd, VIDIOC_QBUF, &buffer) < 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%" PRIu32 "] failed (errno=%d)", i, errno);
            err = ESP_FAIL;
            goto fail;
        }
    }

    if (preview->pixel_format == V4L2_PIX_FMT_JPEG) {
        jpeg_dec_config_t decoder_config = DEFAULT_JPEG_DEC_CONFIG();
        decoder_config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        jpeg_error_t jpeg_err = jpeg_dec_open(&decoder_config, &preview->jpeg_decoder);
        if (jpeg_err != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "MJPEG decoder open failed: %d", jpeg_err);
            err = ESP_FAIL;
            goto fail;
        }
        size_t decoded_size = (size_t)preview->capture_width * preview->capture_height * sizeof(uint16_t);
        preview->jpeg_frame = jpeg_calloc_align(decoded_size, 16);
        if (!preview->jpeg_frame) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
    }

    err = usb_camera_prepare_display(preview);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to prepare display: %s", esp_err_to_name(err));
        goto fail;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(preview->fd, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed (errno=%d)", errno);
        err = ESP_FAIL;
        goto fail;
    }
    preview->stream_started = true;

    struct v4l2_streamparm stream_parm = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };
    uint32_t fps_numerator = 0;
    uint32_t fps_denominator = 0;
    if (ioctl(preview->fd, VIDIOC_G_PARM, &stream_parm) == 0) {
        fps_numerator = stream_parm.parm.capture.timeperframe.denominator;
        fps_denominator = stream_parm.parm.capture.timeperframe.numerator;
    }

    uint32_t capture_width = preview->capture_width;
    uint32_t capture_height = preview->capture_height;
    uint32_t pixel_format = preview->pixel_format;
    uint16_t lcd_width = preview->lcd_width;
    uint16_t lcd_height = preview->lcd_height;
    uint16_t draw_width = preview->draw_width;
    uint16_t draw_height = preview->draw_height;

    printf("[PASS] UVC camera opened: %s, %s %" PRIu32 "x%" PRIu32,
           USB_CAMERA_DEVICE_PATH, usb_camera_format_name(pixel_format), capture_width, capture_height);
    if (fps_denominator != 0) {
        printf(", %.1f FPS", (double)fps_numerator / fps_denominator);
    }
    printf("\nPreview: %ux%u image on %ux%u LCD. Use 'test usb_otg camera stop'.\n",
           draw_width, draw_height, lcd_width, lcd_height);
    return ESP_OK;

fail:
    return err;
}

static esp_err_t usb_camera_stream(usb_camera_preview_t *preview)
{
    uint32_t consecutive_errors = 0;

    while (!preview->stop_requested) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(preview->fd, VIDIOC_DQBUF, &buffer) < 0) {
            int dequeue_errno = errno;
            if (preview->stop_requested) {
                return ESP_OK;
            }
            if (dequeue_errno == ENODEV) {
                return ESP_ERR_NOT_FOUND;
            }
            consecutive_errors++;
            if (consecutive_errors >= USB_CAMERA_MAX_DQBUF_ERRORS) {
                ESP_LOGW(TAG, "UVC frame receive failed %" PRIu32 " times (errno=%d)",
                         consecutive_errors, dequeue_errno);
                return ESP_FAIL;
            }
            continue;
        }
        consecutive_errors = 0;

        esp_err_t frame_err = ESP_OK;
        if (buffer.index >= preview->buffer_count) {
            ESP_LOGE(TAG, "Invalid UVC buffer index: %" PRIu32, buffer.index);
            frame_err = ESP_ERR_INVALID_RESPONSE;
        } else if (preview->pixel_format == V4L2_PIX_FMT_YUYV) {
            size_t required = (size_t)preview->capture_width * preview->capture_height * 2;
            if (buffer.bytesused < required) {
                ESP_LOGW(TAG, "Short YUYV frame: %" PRIu32 "/%zu bytes", buffer.bytesused, required);
                frame_err = ESP_ERR_INVALID_SIZE;
            } else {
                usb_camera_scale_yuyv(preview, preview->buffers[buffer.index].start);
            }
        } else {
            frame_err = usb_camera_decode_mjpeg(preview,
                                                preview->buffers[buffer.index].start,
                                                buffer.bytesused);
            if (frame_err == ESP_OK) {
                usb_camera_scale_rgb565(preview, preview->jpeg_frame);
            }
        }

        if (frame_err == ESP_OK) {
            display_service_raw_blit_t blit = {
                .x_start = 0,
                .y_start = 0,
                .x_end = preview->lcd_width,
                .y_end = preview->lcd_height,
                .frame_buffer = preview->lcd_frame,
                .wait = true,
            };
            frame_err = display_service_session_raw_blit(preview->display_session, &blit);
            if (frame_err == ESP_OK) {
                preview->frames_displayed++;
                if ((preview->frames_displayed % 30) == 0) {
                    ESP_LOGI(TAG, "USB camera frames displayed: %" PRIu32, preview->frames_displayed);
                }
            }
        }
        if (frame_err != ESP_OK) {
            preview->frame_errors++;
        }

        if (ioctl(preview->fd, VIDIOC_QBUF, &buffer) < 0) {
            int queue_errno = errno;
            if (preview->stop_requested) {
                return ESP_OK;
            }
            ESP_LOGW(TAG, "VIDIOC_QBUF failed (errno=%d)", queue_errno);
            return queue_errno == ENODEV ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
    }

    return ESP_OK;
}

static void usb_camera_preview_task(void *arg)
{
    usb_camera_preview_t *preview = (usb_camera_preview_t *)arg;
    uint32_t connection_count = 0;

    ESP_LOGI(TAG, "UVC hot-plug preview task started");
    while (!preview->stop_requested) {
        printf("Waiting for UVC camera on %s; plug in a camera or use 'test usb_otg camera stop'.\n",
               USB_CAMERA_DEVICE_PATH);

        esp_err_t err = usb_camera_connect(preview);
        if (preview->stop_requested) {
            usb_camera_release(preview);
            break;
        }
        if (err != ESP_OK) {
            preview->frame_errors++;
            usb_camera_release(preview);
            ESP_LOGW(TAG, "Camera setup failed: %s; retrying", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(USB_CAMERA_RECONNECT_DELAY_MS));
            continue;
        }

        connection_count++;
        err = usb_camera_stream(preview);
        usb_camera_release(preview);
        if (!preview->stop_requested) {
            if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "UVC camera disconnected; waiting for reconnection");
            } else {
                ESP_LOGW(TAG, "UVC stream stopped: %s; reconnecting", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(USB_CAMERA_RECONNECT_DELAY_MS));
        }
    }

    uint32_t frames_displayed = preview->frames_displayed;
    uint32_t frame_errors = preview->frame_errors;
    usb_camera_release(preview);

    if (frames_displayed == 0) {
        printf("[FAIL] USB camera preview stopped without receiving a frame: connections=%" PRIu32
               ", errors=%" PRIu32 "\n", connection_count, frame_errors);
    } else {
        printf("[PASS] USB camera preview stopped: connections=%" PRIu32 ", frames=%" PRIu32
               ", errors=%" PRIu32 "\n", connection_count, frames_displayed, frame_errors);
    }

    portENTER_CRITICAL(&s_preview_lock);
    if (s_preview == preview) {
        s_preview = NULL;
    }
    portEXIT_CRITICAL(&s_preview_lock);
    free(preview);
    vTaskDelete(NULL);
}

esp_err_t hw_test_usb_camera_start(void)
{
    portENTER_CRITICAL(&s_preview_lock);
    bool busy = s_preview_starting || s_preview != NULL;
    if (!busy) {
        s_preview_starting = true;
    }
    portEXIT_CRITICAL(&s_preview_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_camera_preview_t *preview = calloc(1, sizeof(*preview));
    if (!preview) {
        portENTER_CRITICAL(&s_preview_lock);
        s_preview_starting = false;
        portEXIT_CRITICAL(&s_preview_lock);
        return ESP_ERR_NO_MEM;
    }
    preview->fd = -1;

    portENTER_CRITICAL(&s_preview_lock);
    s_preview = preview;
    s_preview_starting = false;
    portEXIT_CRITICAL(&s_preview_lock);

    if (xTaskCreate(usb_camera_preview_task, "usb_cam_preview", USB_CAMERA_TASK_STACK_SIZE,
                    preview, USB_CAMERA_TASK_PRIORITY, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_preview_lock);
        if (s_preview == preview) {
            s_preview = NULL;
        }
        portEXIT_CRITICAL(&s_preview_lock);
        free(preview);
        return ESP_ERR_NO_MEM;
    }

    printf("[OK] UVC hot-plug preview started. Use 'test usb_otg camera stop' to stop it.\n");
    return ESP_OK;
}

esp_err_t hw_test_usb_camera_stop(void)
{
    portENTER_CRITICAL(&s_preview_lock);
    usb_camera_preview_t *preview = s_preview;
    bool starting = s_preview_starting;
    if (preview) {
        preview->stop_requested = true;
    }
    portEXIT_CRITICAL(&s_preview_lock);

    if (starting && !preview) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!preview) {
        return ESP_OK;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(USB_CAMERA_STOP_TIMEOUT_MS);
    while (true) {
        portENTER_CRITICAL(&s_preview_lock);
        bool stopped = s_preview != preview;
        portEXIT_CRITICAL(&s_preview_lock);
        if (stopped) {
            return ESP_OK;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGE(TAG, "Timed out waiting for USB camera preview to stop");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool hw_test_usb_camera_is_running(void)
{
    portENTER_CRITICAL(&s_preview_lock);
    bool running = s_preview_starting || s_preview != NULL;
    portEXIT_CRITICAL(&s_preview_lock);
    return running;
}
