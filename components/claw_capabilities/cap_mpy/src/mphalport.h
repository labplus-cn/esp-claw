/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MicroPython HAL port header for ESP-Claw cap_mpy component.
 *
 * NOTE: This header is also processed by the qstr preprocessor which may not
 * have ESP-IDF include paths.  All ESP-IDF headers are guarded with
 * __has_include() so the preprocessor can skip them safely.
 */
#pragma once

#include "py/ringbuf.h"
#include "shared/runtime/interrupt_char.h"

/* -- ESP-IDF headers (may not be available during qstr preprocessing) -- */
#if defined(__has_include)
#if __has_include("freertos/FreeRTOS.h")
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define _MP_HAS_FREERTOS 1
#endif
#if __has_include("esp_timer.h")
#include "esp_timer.h"
#define _MP_HAS_ESP_TIMER 1
#endif
#if __has_include("esp_rom_sys.h")
#include "esp_rom_sys.h"
#define _MP_HAS_ESP_ROM 1
#endif
#if __has_include("driver/gpio.h")
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#if __has_include("soc/gpio_reg.h")
#include "soc/gpio_reg.h"
#endif
#define _MP_HAS_GPIO 1
#endif
#endif /* __has_include */

/* Provide fallback types when ESP-IDF headers are not available (qstr scan) */
#ifndef _MP_HAS_FREERTOS
typedef unsigned int TaskHandle_t;
typedef unsigned int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED  {}
#define portENTER_CRITICAL(m)  (void)0
#define portEXIT_CRITICAL(m)   (void)0
#define pdMS_TO_TICKS(ms)      (ms)
#define portTICK_PERIOD_MS     1
#endif

#ifdef _MP_HAS_FREERTOS
#define MICROPY_PLATFORM_VERSION   "IDF" IDF_VER
#else
#define MICROPY_PLATFORM_VERSION   "IDF"
#endif

extern TaskHandle_t mp_main_task_handle;
extern ringbuf_t stdin_ringbuf;
extern portMUX_TYPE mp_atomic_mux;

/* Atomic section */
static inline unsigned int mp_begin_atomic_section(void)
{
    portENTER_CRITICAL(&mp_atomic_mux);
    return 0;
}

static inline void mp_end_atomic_section(unsigned int state)
{
    (void)state;
    portEXIT_CRITICAL(&mp_atomic_mux);
}

#define MICROPY_BEGIN_ATOMIC_SECTION()   mp_begin_atomic_section()
#define MICROPY_END_ATOMIC_SECTION(state) mp_end_atomic_section(state)

/* Timing */
#ifdef _MP_HAS_ESP_TIMER
__attribute__((always_inline)) static inline mp_uint_t mp_hal_ticks_us(void)
{
    return esp_timer_get_time();
}
#endif

__attribute__((always_inline)) static inline mp_uint_t mp_hal_ticks_cpu(void)
{
    return 0;
}

#define mp_hal_quiet_timing_enter()   MICROPY_BEGIN_ATOMIC_SECTION()
#define mp_hal_quiet_timing_exit(irq_state) MICROPY_END_ATOMIC_SECTION(irq_state)

void mp_hal_delay_us(mp_uint_t delay);
void mp_hal_delay_ms(mp_uint_t delay);
#ifdef _MP_HAS_ESP_ROM
#define mp_hal_delay_us_fast(us)   esp_rom_delay_us(us)
#else
#define mp_hal_delay_us_fast(us)   (void)0
#endif

/* Stdio */
int mp_hal_stdin_rx_chr(void);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
void mp_hal_uart_open_repl(void);
void mp_hal_uart_close(void);

/* Time */
uint64_t mp_hal_time_ns(void);

/* Interrupt */
void mp_hal_set_interrupt_char(int c);

/* Random */
void mp_hal_get_random(size_t n, uint8_t *buf);

/* Error checking */
void check_esp_err_(int code);
#define check_esp_err(code) check_esp_err_(code)

/* Retry syscall on EINTR (needed by extmod/vfs_posix.c) */
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) { \
    for (;;) { \
        ret = syscall; \
        if (ret == -1) { \
            int err = errno; \
            if (err == EINTR) { continue; } \
            raise; \
        } \
        break; \
    } \
}

/* -- Wake main task (needed by Pin IRQ) -- */
void mp_hal_wake_main_task(void);
void mp_hal_wake_main_task_from_isr(void);

/* -- C-level Pin HAL (needed by extmod/machine_i2c.c, machine_pin.c) -- */
#ifdef _MP_HAS_GPIO
#include "py/obj.h"
#define MP_HAL_PIN_FMT                      "%u"
#define mp_hal_pin_obj_t                    gpio_num_t
mp_hal_pin_obj_t machine_pin_get_id(mp_obj_t pin_in);
#define mp_hal_get_pin_obj(o)              machine_pin_get_id(o)
#define mp_hal_pin_name(p)                 (p)
static inline void mp_hal_pin_input(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}
static inline void mp_hal_pin_output(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
}
static inline void mp_hal_pin_open_drain(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}
static inline void mp_hal_pin_od_low(mp_hal_pin_obj_t pin) {
    gpio_set_level(pin, 0);
}
static inline void mp_hal_pin_od_high(mp_hal_pin_obj_t pin) {
    gpio_set_level(pin, 1);
}
static inline int mp_hal_pin_read(mp_hal_pin_obj_t pin) {
    return gpio_get_level(pin);
}
static inline int mp_hal_pin_read_output(mp_hal_pin_obj_t pin) {
    #if defined(GPIO_OUT1_REG)
    return pin < 32
        ? (*(uint32_t *)GPIO_OUT_REG >> pin) & 1
        : (*(uint32_t *)GPIO_OUT1_REG >> (pin - 32)) & 1;
    #else
    return (*(uint32_t *)GPIO_OUT_REG >> pin) & 1;
    #endif
}
static inline void mp_hal_pin_write(mp_hal_pin_obj_t pin, int v) {
    gpio_set_level(pin, v);
}
#endif /* _MP_HAS_GPIO */
