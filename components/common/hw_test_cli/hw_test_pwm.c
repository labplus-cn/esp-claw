/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PWM connectivity test — toggles GPIO level every second (true 1 Hz) on a
 * fixed set of GPIOs so the operator can verify pin continuity through
 * connected LEDs.  Uses a lightweight FreeRTOS software timer; no LEDC
 * hardware involved, so there is no conflict with SDIO / other peripherals.
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "test_pwm";

/* ---- Target GPIOs for connectivity test ---- */
static const int s_pwm_gpios[] = {
    51, 52, 53, 54,           /* IO51-54  */
    45, 46, 47, 48,           /* IO45-48  */
};
#define PWM_GPIO_COUNT  (sizeof(s_pwm_gpios) / sizeof(s_pwm_gpios[0]))

/* ---- Runtime state ---- */
static TimerHandle_t s_toggle_timer = NULL;
static bool          s_level        = true;   /* current output level */

/* ---- Timer callback: flip all GPIOs every 1 s ---- */
static void pwm_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    s_level = !s_level;

    for (int i = 0; i < (int)PWM_GPIO_COUNT; i++) {
        gpio_set_level(s_pwm_gpios[i], s_level ? 1 : 0);
    }

    ESP_LOGI(TAG, "GPIO %s", s_level ? "HIGH" : "LOW");
}

/* ---- Start ---- */
static int pwm_start(void)
{
    if (s_toggle_timer) {
        printf("PWM test already running.  Use: test pwm stop\n");
        return 0;
    }

    /* 1. Configure all GPIOs as push-pull output, initial level HIGH */
    for (int i = 0; i < (int)PWM_GPIO_COUNT; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << s_pwm_gpios[i]),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            printf("[FAIL] GPIO %d config: %s\n",
                   s_pwm_gpios[i], esp_err_to_name(err));
            /* Roll back already-configured pins */
            for (int j = 0; j < i; j++) {
                gpio_reset_pin(s_pwm_gpios[j]);
            }
            return 0;
        }
        gpio_set_level(s_pwm_gpios[i], 1);
    }
    s_level = true;

    /* 2. Create 1-second periodic software timer */
    s_toggle_timer = xTimerCreate("pwm_test",
                                  pdMS_TO_TICKS(1000),
                                  pdTRUE,          /* auto-reload */
                                  NULL,
                                  pwm_timer_cb);
    if (!s_toggle_timer) {
        printf("[FAIL] Cannot create toggle timer\n");
        for (int i = 0; i < (int)PWM_GPIO_COUNT; i++) {
            gpio_reset_pin(s_pwm_gpios[i]);
        }
        return 0;
    }

    if (xTimerStart(s_toggle_timer, 0) != pdPASS) {
        printf("[FAIL] Cannot start toggle timer\n");
        xTimerDelete(s_toggle_timer, 0);
        s_toggle_timer = NULL;
        for (int i = 0; i < (int)PWM_GPIO_COUNT; i++) {
            gpio_reset_pin(s_pwm_gpios[i]);
        }
        return 0;
    }

    printf("PWM test started — 1 Hz toggle on %d pins (", (int)PWM_GPIO_COUNT);
    for (int i = 0; i < (int)PWM_GPIO_COUNT; i++) {
        printf("%d%s", s_pwm_gpios[i],
               (i < (int)PWM_GPIO_COUNT - 1) ? ", " : "");
    }
    printf(")\n");
    printf("  Use 'test pwm stop' to turn off\n");
    return 0;
}

/* ---- Stop ---- */
static int pwm_stop(void)
{
    if (!s_toggle_timer) {
        printf("PWM test is not running.\n");
        return 0;
    }

    xTimerStop(s_toggle_timer, 0);
    xTimerDelete(s_toggle_timer, 0);
    s_toggle_timer = NULL;

    /* Turn off all outputs and release GPIOs */
    for (int i = 0; i < (int)PWM_GPIO_COUNT; i++) {
        gpio_set_level(s_pwm_gpios[i], 0);
        gpio_reset_pin(s_pwm_gpios[i]);
    }

    printf("PWM test stopped — all outputs off\n");
    return 0;
}

/* ---- Entry point ---- */
int test_pwm(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  test pwm start   Start 1 Hz GPIO toggle on test pins\n");
        printf("  test pwm stop    Stop and release GPIOs\n");
        printf("\nGPIOs: ");
        for (int i = 0; i < (int)PWM_GPIO_COUNT; i++) {
            printf("%d%s", s_pwm_gpios[i],
                   (i < (int)PWM_GPIO_COUNT - 1) ? ", " : "");
        }
        printf("\n");
        return 0;
    }

    if (strcmp(argv[1], "start") == 0) {
        return pwm_start();
    } else if (strcmp(argv[1], "stop") == 0) {
        return pwm_stop();
    }

    printf("Unknown pwm sub-command: '%s'\n", argv[1]);
    printf("Use 'test pwm' to see usage.\n");
    return 0;
}
