/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_button";

/* Default GPIO and timing for the confirm button (labplus_ledong_max_v1).
 * Override at runtime: test button --gpio 35 --active 0 --timeout 15 */
#define TEST_BUTTON_DEFAULT_GPIO       35
#define TEST_BUTTON_DEFAULT_ACTIVE     0
#define TEST_BUTTON_DEFAULT_TIMEOUT_S  10
#define TEST_BUTTON_POLL_MS            20

int test_button(int argc, char **argv)
{
    int gpio_num = TEST_BUTTON_DEFAULT_GPIO;
    int active_level = TEST_BUTTON_DEFAULT_ACTIVE;
    int timeout_s = TEST_BUTTON_DEFAULT_TIMEOUT_S;

    /* Parse optional arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gpio") == 0 && i + 1 < argc) {
            gpio_num = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--active") == 0 && i + 1 < argc) {
            active_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: test button [options]\n");
            printf("  --gpio <num>      GPIO number (default: %d)\n", TEST_BUTTON_DEFAULT_GPIO);
            printf("  --active <0|1>    Active level (default: %d)\n", TEST_BUTTON_DEFAULT_ACTIVE);
            printf("  --timeout <sec>   Wait timeout in seconds (default: %d)\n", TEST_BUTTON_DEFAULT_TIMEOUT_S);
            return 0;
        }
    }

    /* Configure GPIO as input */
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

    int initial_level = gpio_get_level(gpio_num);
    printf("Button test: GPIO=%d active=%d timeout=%ds initial_level=%d\n",
           gpio_num, active_level, timeout_s, initial_level);
    printf("Press the confirm button... (Ctrl+C to abort)\n");

    int press_count = 0;
    int last_level = initial_level;
    int timeout_ticks = (timeout_s * 1000) / TEST_BUTTON_POLL_MS;

    for (int i = 0; i < timeout_ticks; i++) {
        int level = gpio_get_level(gpio_num);

        if (level != last_level) {
            if (level == active_level) {
                press_count++;
                printf("[PASS] Press #%d detected at %d ms\n",
                       press_count, i * TEST_BUTTON_POLL_MS);
            } else {
                printf("       Release at %d ms\n", i * TEST_BUTTON_POLL_MS);
            }
            last_level = level;
        }

        if (press_count >= 1) {
            /* Got at least one press — wait for release then report */
            vTaskDelay(pdMS_TO_TICKS(100));
            printf("\n=== Button test PASSED (%d press(es) detected) ===\n", press_count);
            return 0;
        }

        vTaskDelay(pdMS_TO_TICKS(TEST_BUTTON_POLL_MS));
    }

    printf("\n=== Button test FAILED (no press detected within %ds) ===\n", timeout_s);
    return 1;
}
