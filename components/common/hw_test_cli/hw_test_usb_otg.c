/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CLI test commands for USB OTG role switching.
 *
 *   test usb_otg status    — show current role
 *   test usb_otg host      — switch to Host mode (UVC camera)
 *   test usb_otg device    — switch to Device mode (MSC U-disk)
 *   test usb_otg camera    — preview an external UVC camera on the LCD
 *   test usb_otg camera stop — stop camera preview and release USB
 *   test usb_otg stop      — stop current role, release USB
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hw_test_usb_camera.h"
#include "usb_otg.h"

static const char *TAG = "test_usb_otg";

static const char *role_name(usb_otg_role_t role)
{
    switch (role) {
    case USB_OTG_ROLE_NONE:   return "NONE";
    case USB_OTG_ROLE_HOST:   return "HOST (UVC camera)";
    case USB_OTG_ROLE_DEVICE: return "DEVICE (MSC U-disk)";
    default:                  return "UNKNOWN";
    }
}

static int cmd_status(void)
{
    usb_otg_role_t role = usb_otg_get_role();
    printf("USB OTG role: %s\n", role_name(role));
    printf("USB camera preview: %s\n", hw_test_usb_camera_is_running() ? "RUNNING" : "STOPPED");
    return 0;
}

static esp_err_t stop_camera_preview(void)
{
    if (!hw_test_usb_camera_is_running()) {
        return ESP_OK;
    }

    printf("Stopping USB camera preview...\n");
    return hw_test_usb_camera_stop();
}

static int cmd_host(void)
{
    esp_err_t err = usb_otg_init();
    if (err != ESP_OK) {
        printf("[FAIL] usb_otg_init: %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("Switching to HOST mode (UVC camera)...\n");
    err = usb_otg_switch_role(USB_OTG_ROLE_HOST);
    if (err != ESP_OK) {
        printf("[FAIL] switch to HOST: %s\n", esp_err_to_name(err));
    } else {
        printf("USB OTG is now in HOST mode.\n");
    }
    return 0;
}

static int cmd_device(void)
{
    esp_err_t err = stop_camera_preview();
    if (err != ESP_OK) {
        printf("[FAIL] camera stop: %s\n", esp_err_to_name(err));
        return 0;
    }

    err = usb_otg_init();
    if (err != ESP_OK) {
        printf("[FAIL] usb_otg_init: %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("Switching to DEVICE mode (MSC U-disk)...\n");
    err = usb_otg_switch_role(USB_OTG_ROLE_DEVICE);
    if (err != ESP_OK) {
        printf("[FAIL] switch to DEVICE: %s\n", esp_err_to_name(err));
    } else {
        printf("USB OTG is now in DEVICE mode. SD card exposed as USB U-disk.\n");
    }
    return 0;
}

static int cmd_stop(void)
{
    printf("Stopping USB OTG...\n");
    esp_err_t err = stop_camera_preview();
    if (err != ESP_OK) {
        printf("[FAIL] camera stop: %s\n", esp_err_to_name(err));
        return 0;
    }
    err = usb_otg_switch_role(USB_OTG_ROLE_NONE);
    if (err != ESP_OK) {
        printf("[FAIL] stop: %s\n", esp_err_to_name(err));
    } else {
        printf("USB OTG stopped.\n");
    }
    return 0;
}

static int cmd_camera(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[2], "stop") == 0) {
        esp_err_t err = stop_camera_preview();
        if (err != ESP_OK) {
            printf("[FAIL] camera stop: %s\n", esp_err_to_name(err));
            return 0;
        }
        err = usb_otg_switch_role(USB_OTG_ROLE_NONE);
        if (err != ESP_OK) {
            printf("[FAIL] USB Host stop: %s\n", esp_err_to_name(err));
        } else {
            printf("USB camera and Host mode stopped.\n");
        }
        return 0;
    }

    if (argc > 2) {
        printf("Usage: test usb_otg camera [stop]\n");
        return 0;
    }
    if (hw_test_usb_camera_is_running()) {
        printf("USB camera preview is already running.\n");
        return 0;
    }

    esp_err_t err = usb_otg_init();
    if (err != ESP_OK) {
        printf("[FAIL] usb_otg_init: %s\n", esp_err_to_name(err));
        return 0;
    }

    usb_otg_role_t previous_role = usb_otg_get_role();
    printf("Starting USB Host and UVC camera preview...\n");
    err = usb_otg_switch_role(USB_OTG_ROLE_HOST);
    if (err != ESP_OK) {
        printf("[FAIL] switch to HOST: %s\n", esp_err_to_name(err));
        return 0;
    }

    err = hw_test_usb_camera_start();
    if (err != ESP_OK) {
        printf("[FAIL] UVC camera preview: %s\n", esp_err_to_name(err));
        if (previous_role != USB_OTG_ROLE_HOST) {
            esp_err_t stop_err = usb_otg_switch_role(USB_OTG_ROLE_NONE);
            if (stop_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to release Host mode after camera error: %s",
                         esp_err_to_name(stop_err));
            }
        }
    }
    return 0;
}

int test_usb_otg(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: test usb_otg <sub-command>\n");
        printf("Sub-commands:\n");
        printf("  status    Show current USB OTG role\n");
        printf("  host      Switch to Host mode (UVC camera)\n");
        printf("  device    Switch to Device mode (MSC U-disk)\n");
        printf("  camera    Start UVC camera preview on LCD\n");
        printf("  camera stop  Stop preview and release USB Host\n");
        printf("  stop      Stop current role, release USB\n");
        return 0;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "status") == 0) {
        return cmd_status();
    } else if (strcmp(sub, "host") == 0) {
        return cmd_host();
    } else if (strcmp(sub, "device") == 0) {
        return cmd_device();
    } else if (strcmp(sub, "camera") == 0) {
        return cmd_camera(argc, argv);
    } else if (strcmp(sub, "stop") == 0) {
        return cmd_stop();
    }

    printf("Unknown usb_otg sub-command: '%s'\n", sub);
    return 0;
}
