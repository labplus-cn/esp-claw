/*
 * MicroPython HAL port for ESP-Claw cap_mpy component.
 *
 * Based on the official MicroPython ESP32 port mphalport.c, with:
 * - TinyUSB / USB Serial JTAG removed (not needed for embedded use)
 * - UART fd support for REPL takeover (mp_hal_stdin_rx_chr reads from
 *   /dev/console when REPL is active, falls back to ringbuf otherwise)
 * - mp_hal_uart_close() for releasing UART after REPL exit
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_system.h"

#include "py/obj.h"
#include "py/objstr.h"
#include "py/stream.h"
#include "py/mpstate.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/timeutils/timeutils.h"
#include "shared/runtime/pyexec.h"

#include "mphalport.h"

static const char *TAG = "mpy_hal";

TaskHandle_t mp_main_task_handle;

static uint8_t stdin_ringbuf_array[260];
ringbuf_t stdin_ringbuf = {stdin_ringbuf_array, sizeof(stdin_ringbuf_array), 0, 0};

portMUX_TYPE mp_atomic_mux = portMUX_INITIALIZER_UNLOCKED;

/* UART fd for REPL direct access (-1 = not open, use ringbuf) */
static int s_uart_fd = -1;

/* ── Error checking ─────────────────────────────────────────────────── */
void check_esp_err_(esp_err_t code)
{
    if (code != ESP_OK) {
        uint32_t pcode = -code;
        switch (code) {
            case ESP_ERR_NO_MEM:     pcode = MP_ENOMEM; break;
            case ESP_ERR_TIMEOUT:    pcode = MP_ETIMEDOUT; break;
            case ESP_ERR_NOT_SUPPORTED: pcode = MP_EOPNOTSUPP; break;
        }
        mp_obj_str_t *o_str = m_new_obj_maybe(mp_obj_str_t);
        if (o_str == NULL) {
            mp_raise_OSError(pcode);
            return;
        }
        o_str->base.type = &mp_type_str;
        o_str->data = (const byte *)esp_err_to_name(code);
        o_str->len = strlen((char *)o_str->data);
        o_str->hash = qstr_compute_hash(o_str->data, o_str->len);
        mp_obj_t args[2] = { MP_OBJ_NEW_SMALL_INT(pcode), MP_OBJ_FROM_PTR(o_str) };
        nlr_raise(mp_obj_exception_make_new(&mp_type_OSError, 2, 0, args));
    }
}

/* ── Stdio poll ─────────────────────────────────────────────────────── */
uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags)
{
    uintptr_t ret = 0;
    if ((poll_flags & MP_STREAM_POLL_RD) && ringbuf_peek(&stdin_ringbuf) != -1) {
        ret |= MP_STREAM_POLL_RD;
    }
    if (poll_flags & MP_STREAM_POLL_WR) {
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}

/* ── Stdin ──────────────────────────────────────────────────────────── */
int mp_hal_stdin_rx_chr(void)
{
    for (;;) {
        /* If REPL has taken over UART, read directly from fd */
        if (s_uart_fd >= 0) {
            uint8_t c;
            int n = read(s_uart_fd, &c, 1);
            if (n == 1) {
                return c;
            }
            MICROPY_EVENT_POLL_HOOK
            continue;
        }
        /* Normal mode: read from ringbuf */
        int c = ringbuf_get(&stdin_ringbuf);
        if (c != -1) {
            return c;
        }
        MICROPY_EVENT_POLL_HOOK
    }
}

/* ── Stdout ─────────────────────────────────────────────────────────── */
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    mp_uint_t ret = 0;
    /* If REPL has taken over UART, write directly to fd */
    if (s_uart_fd >= 0) {
        int written = write(s_uart_fd, str, len);
        if (written > 0) {
            fsync(s_uart_fd);
        }
        return written > 0 ? (mp_uint_t)written : 0;
    }
    /* Normal mode: write to stdout (which goes to esp_console/UART) */
    ret = fwrite(str, 1, len, stdout);
    return ret;
}

/* ── UART close (called when REPL exits) ────────────────────────────── */
void mp_hal_uart_close(void)
{
    if (s_uart_fd >= 0) {
        close(s_uart_fd);
        s_uart_fd = -1;
    }
}

/* ── REPL open (called when REPL starts) ────────────────────────────── */
void mp_hal_uart_open_repl(void)
{
    if (s_uart_fd < 0) {
        s_uart_fd = open("/dev/console", O_RDWR);
        if (s_uart_fd < 0) {
            ESP_LOGE(TAG, "Failed to open /dev/console for REPL");
        }
    }
}

/* ── Timing ─────────────────────────────────────────────────────────── */
mp_uint_t mp_hal_ticks_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void mp_hal_delay_ms(mp_uint_t ms)
{
    uint64_t us = (uint64_t)ms * 1000ULL;
    uint64_t dt;
    uint64_t t0 = esp_timer_get_time();
    for (;;) {
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
        MP_THREAD_GIL_EXIT();
        uint64_t t1 = esp_timer_get_time();
        dt = t1 - t0;
        if (dt + portTICK_PERIOD_MS * 1000ULL >= us) {
            taskYIELD();
            MP_THREAD_GIL_ENTER();
            t1 = esp_timer_get_time();
            dt = t1 - t0;
            break;
        } else {
            ulTaskNotifyTake(pdFALSE, 1);
            MP_THREAD_GIL_ENTER();
        }
    }
    if (dt < us) {
        mp_hal_delay_us(us - dt);
    }
}

void mp_hal_delay_us(mp_uint_t us)
{
    if (us < 5) {
        return;
    }
    us -= 5;
    uint64_t t0 = esp_timer_get_time();
    for (;;) {
        uint64_t dt = esp_timer_get_time() - t0;
        if (dt >= us) {
            return;
        }
        if (dt + 150 < us) {
            mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
        }
    }
}

uint64_t mp_hal_time_ns(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ns = tv.tv_sec * 1000000000ULL;
    ns += (uint64_t)tv.tv_usec * 1000ULL;
    return ns;
}

/* ── Wake main task ─────────────────────────────────────────────────── */
void mp_hal_wake_main_task(void)
{
    xTaskNotifyGive(mp_main_task_handle);
}

void mp_hal_wake_main_task_from_isr(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(mp_main_task_handle, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ── Random ─────────────────────────────────────────────────────────── */
void mp_hal_get_random(size_t n, uint8_t *buf)
{
    uint32_t r = 0;
    for (size_t i = 0; i < n; i++) {
        if ((i & 3) == 0) {
            r = esp_random();
        }
        buf[i] = r;
        r >>= 8;
    }
}

/* ── NLR fail ───────────────────────────────────────────────────────── */
void nlr_jump_fail(void *val)
{
    ESP_LOGE("mpy", "NLR jump failed, val=%p", val);
    esp_restart();
}
