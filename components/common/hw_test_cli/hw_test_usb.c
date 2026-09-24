/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_usb";

#define TEST_USB_DEFAULT_TIMEOUT_S  30
#define TEST_USB_DEFAULT_POLL_MS    50

/*
 * Mirror of the auto-generated board config struct for the gpio_vbus device.
 * MUST be byte-for-byte identical to dev_custom_gpio_vbus_config_t.
 */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int gpio;
    int active_level;
    int debounce_ms;
    const char *description;
    uint8_t peripheral_count;
    const char *peripheral_name;
} usb_vbus_board_cfg_t;

/**
 * Try to read GPIO / active_level from the board manager "vbus_detector" device.
 * Returns ESP_OK on success, or ESP_ERR_NOT_FOUND if the device is not configured.
 */
static esp_err_t usb_vbus_read_board_cfg(int *out_gpio, int *out_active)
{
    void *config = NULL;
    esp_err_t err = esp_board_manager_get_device_config("vbus_detector", &config);
    if (err != ESP_OK || !config) {
        return err ? err : ESP_ERR_NOT_FOUND;
    }

    usb_vbus_board_cfg_t *cfg = (usb_vbus_board_cfg_t *)config;
    if (cfg->gpio >= 0) {
        *out_gpio = cfg->gpio;
    }
    *out_active = cfg->active_level;

    ESP_LOGI(TAG, "board vbus_detector: gpio=%d, active_level=%d, desc='%s'",
             cfg->gpio, cfg->active_level, cfg->description ? cfg->description : "");
    return ESP_OK;
}

int test_usb(int argc, char **argv)
{
    int gpio_num = -1;
    int active_level = -1;
    int timeout_s = TEST_USB_DEFAULT_TIMEOUT_S;
    bool monitor_mode = false;
    bool from_board = false;

    /* Parse optional arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gpio") == 0 && i + 1 < argc) {
            gpio_num = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--active") == 0 && i + 1 < argc) {
            active_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--monitor") == 0 || strcmp(argv[i], "-m") == 0) {
            monitor_mode = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: test usb [options]\n");
            printf("  --gpio <num>      GPIO number (override board config)\n");
            printf("  --active <0|1>    Active level: 0=LOW, 1=HIGH when USB plugged (override board config)\n");
            printf("  --timeout <sec>   Monitor timeout in seconds (default: %d)\n", TEST_USB_DEFAULT_TIMEOUT_S);
            printf("  --monitor, -m     Continuous monitor for plug/unplug events\n");
            printf("\nIf --gpio / --active are not given, reads from board manager 'vbus_detector' device.\n");
            return 0;
        }
    }

    /* If not overridden on command line, read from board manager */
    if (gpio_num < 0 || active_level < 0) {
        int board_gpio = -1;
        int board_active = -1;
        esp_err_t err = usb_vbus_read_board_cfg(&board_gpio, &board_active);
        if (err == ESP_OK) {
            from_board = true;
            if (gpio_num < 0) {
                gpio_num = board_gpio;
            }
            if (active_level < 0) {
                active_level = board_active;
            }
        }
    }

    /* Apply fallback defaults if board config not available */
    if (gpio_num < 0) {
        gpio_num = 13;  /* Last-resort fallback */
    }
    if (active_level < 0) {
        active_level = 0;  /* Last-resort fallback */
    }

    printf("USB detect: GPIO=%d, active=%d (%s)\n",
           gpio_num, active_level,
           from_board ? "from board config" : "manual/default");

    /* Configure GPIO as input with pull-down (active-HIGH) or pull-up (active-LOW) */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (active_level == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (active_level != 0) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        printf("GPIO config failed: %s\n", esp_err_to_name(err));
        return 0;
    }

    int level = gpio_get_level(gpio_num);
    bool usb_present = (level == active_level);

    /* One-shot mode: just report current state */
    if (!monitor_mode) {
        printf("USB detect: GPIO=%d level=%d → USB %s\n",
               gpio_num, level, usb_present ? "PLUGGED IN" : "NOT connected");
        return usb_present ? 0 : 1;
    }

    /* Monitor mode: watch for plug/unplug transitions */
    printf("USB monitor: GPIO=%d active=%d timeout=%ds initial=%s\n",
           gpio_num, active_level, timeout_s,
           usb_present ? "PLUGGED IN" : "NOT connected");
    printf("Monitoring plug/unplug events... (Ctrl+C to abort)\n");

    int plug_count = 0;
    int unplug_count = 0;
    int last_level = level;
    int timeout_ticks = (timeout_s * 1000) / TEST_USB_DEFAULT_POLL_MS;

    for (int i = 0; i < timeout_ticks; i++) {
        vTaskDelay(pdMS_TO_TICKS(TEST_USB_DEFAULT_POLL_MS));
        int cur = gpio_get_level(gpio_num);

        if (cur != last_level) {
            int elapsed_ms = i * TEST_USB_DEFAULT_POLL_MS;
            if (cur == active_level) {
                plug_count++;
                printf("[PLUG]   #%d at %d ms  (level=%d)\n", plug_count, elapsed_ms, cur);
            } else {
                unplug_count++;
                printf("[UNPLUG] #%d at %d ms  (level=%d)\n", unplug_count, elapsed_ms, cur);
            }
            last_level = cur;
        }
    }

    /* Final state */
    level = gpio_get_level(gpio_num);
    usb_present = (level == active_level);

    printf("\n=== USB monitor summary ===\n");
    printf("  Plugs:   %d\n", plug_count);
    printf("  Unplugs: %d\n", unplug_count);
    printf("  Final state: %s\n", usb_present ? "PLUGGED IN" : "NOT connected");

    if (plug_count >= 1) {
        printf("=== USB detect test PASSED ===\n");
        return 0;
    } else {
        printf("=== USB detect test FAILED (no plug event detected within %ds) ===\n", timeout_s);
        return 1;
    }
}
