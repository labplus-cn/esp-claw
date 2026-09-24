/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB OTG Host mode — USB Host stack + UVC camera enumeration.
 *
 * Reference: boards/espressif/esp32_S3_DevKitC_1_breadboard/setup_device.c
 */

#include "usb_otg.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_otg_host";

#define USB_HOST_TASK_PRIORITY   5
#define USB_HOST_STOP_TIMEOUT_MS 5000
#define USB_UVC_DEV_NUM          1
#define USB_UVC_TASK_PRIORITY    (configMAX_PRIORITIES - 2)
#define USB_UVC_TASK_STACK_SIZE  4096

static TaskHandle_t       s_host_task_handle;
static volatile bool      s_host_running;
static volatile bool      s_host_installed;
static volatile esp_err_t s_host_task_result;

/* ---- USB Host library background task ---- */

static void usb_host_lib_task(void *arg)
{
    esp_err_t task_result = ESP_OK;

    while (true) {
        uint32_t event_flags = 0;
        esp_err_t ret = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB host event handling failed: %s", esp_err_to_name(ret));
            task_result = ret;
            break;
        }

        bool stopping = !s_host_running;
        if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) || stopping) {
            ret = usb_host_device_free_all();
            if (ret == ESP_OK) {
                if (stopping) {
                    break;
                }
            } else if (ret != ESP_ERR_NOT_FINISHED) {
                ESP_LOGE(TAG, "Failed to free USB devices: %s", esp_err_to_name(ret));
                if (stopping) {
                    task_result = ret;
                    break;
                }
            }
        }
        if (stopping && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) {
            ESP_LOGI(TAG, "USB host freed all devices");
            break;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB host freed all devices");
        }
    }

    if (task_result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        task_result = usb_host_uninstall();
        if (task_result != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_uninstall failed: %s", esp_err_to_name(task_result));
        } else {
            ESP_LOGI(TAG, "USB host uninstalled");
            s_host_installed = false;
        }
    }

    s_host_task_result = task_result;
    s_host_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t usb_host_wait_for_task(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(USB_HOST_STOP_TIMEOUT_MS);

    while (s_host_task_handle != NULL) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return s_host_task_result;
}

static esp_err_t usb_host_stop_stack(void)
{
    s_host_running = false;

    if (s_host_task_handle != NULL) {
        esp_err_t ret = usb_host_lib_unblock();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to unblock USB host task: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = usb_host_wait_for_task();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB host task did not stop cleanly: %s", esp_err_to_name(ret));
            return ret;
        }
    } else if (s_host_installed) {
        esp_err_t ret = usb_host_uninstall();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_uninstall retry failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_host_installed = false;
    }

    return ESP_OK;
}

/* ---- Role start / stop (called by usb_otg_core.c) ---- */

esp_err_t usb_otg_host_start(usb_phy_handle_t phy)
{
    (void)phy;

    if (s_host_installed || s_host_task_handle != NULL) {
        ESP_LOGW(TAG, "USB Host already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_host_running = true;
    s_host_task_result = ESP_OK;

    /* Install USB Host stack — PHY is already configured by usb_otg_core.c */
    const usb_host_config_t host_config = {
        .skip_phy_setup = true,   /* PHY managed externally */
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(ret));
        s_host_running = false;
        return ret;
    }
    s_host_installed = true;

    /* Create background task for USB Host library events */
    BaseType_t task_ret = xTaskCreatePinnedToCore(usb_host_lib_task, "usb_otg_host",
                                                   CONFIG_USB_OTG_HOST_TASK_STACK_SIZE,
                                                   NULL, USB_HOST_TASK_PRIORITY,
                                                   &s_host_task_handle, 0);
    if (task_ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB host task");
        (void)usb_host_uninstall();
        s_host_installed = false;
        s_host_running = false;
        return ESP_ERR_NO_MEM;
    }

    /* Initialize UVC video device */
    const esp_video_init_usb_uvc_config_t uvc_config = {
        .uvc = {
            .uvc_dev_num = USB_UVC_DEV_NUM,
            .task_stack = USB_UVC_TASK_STACK_SIZE,
            .task_priority = USB_UVC_TASK_PRIORITY,
            .task_affinity = 0,
        },
        .usb = {
            .init_usb_host_lib = false,  /* We manage it ourselves */
        },
    };
    const esp_video_init_config_t video_config = {
        .usb_uvc = &uvc_config,
    };

    ret = esp_video_init_with_flags(&video_config, ESP_VIDEO_INIT_FLAGS_USB_UVC);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UVC video init failed: %s", esp_err_to_name(ret));
        esp_err_t stop_ret = usb_host_stop_stack();
        if (stop_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to unwind USB Host after UVC init error: %s",
                     esp_err_to_name(stop_ret));
        }
        return ret;
    }
    ESP_LOGI(TAG, "UVC camera initialized, dev_path: %s",
             ESP_VIDEO_USB_UVC_NAME(0));

    ESP_LOGI(TAG, "USB Host mode started (UVC camera ready)");
    return ESP_OK;
}

esp_err_t usb_otg_host_stop(void)
{
    if (!s_host_running && !s_host_installed && s_host_task_handle == NULL) {
        return ESP_OK;
    }

    /* Deinit UVC video */
    esp_err_t ret = esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_USB_UVC);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = usb_host_stop_stack();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "USB Host mode stopped");
    return ESP_OK;
}
