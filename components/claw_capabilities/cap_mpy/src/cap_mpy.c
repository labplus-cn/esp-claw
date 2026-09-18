/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MicroPython runtime for ESP-Claw cap_mpy component.
 * Handles interpreter init/deinit and synchronous script execution.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/mpstate.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"
#include "shared/runtime/pyexec.h"

#include "mphalport.h"
#include "cap_mpy.h"

static const char *TAG = "cap_mpy";

#define CAP_MPY_GC_HEAP_SIZE    (128 * 1024)
#define CAP_MPY_MAX_SCRIPT_SIZE (64 * 1024)
#define CAP_MPY_DEFAULT_TIMEOUT_MS 60000

static bool s_initialized = false;
static void *s_gc_heap = NULL;

/* ------------------------------------------------------------------ */
/*  MicroPython runtime init / deinit                                  */
/* ------------------------------------------------------------------ */

esp_err_t cap_mpy_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* Allocate GC heap (prefer PSRAM if available) */
    s_gc_heap = heap_caps_malloc(CAP_MPY_GC_HEAP_SIZE, MALLOC_CAP_DEFAULT);
    if (!s_gc_heap) {
        ESP_LOGE(TAG, "Failed to allocate MicroPython GC heap (%d bytes)", CAP_MPY_GC_HEAP_SIZE);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize GC heap FIRST — mp_init() allocates objects via GC */
    gc_init(s_gc_heap, (void *)((uint8_t *)s_gc_heap + CAP_MPY_GC_HEAP_SIZE));

    /* Initialize stack control */
    mp_stack_ctrl_init();

    /* Initialize MicroPython runtime (QSTR pool, builtins, sys modules) */
    mp_init();

    /* Mount POSIX VFS for /fatfs and /sdcard so Python scripts can
     * use open(), os.listdir(), os.stat() etc. on ESP-IDF filesystems. */
    nlr_buf_t nlr_vfs;
    if (nlr_push(&nlr_vfs) == 0) {
        struct stat st_check;

        /* Mount /fatfs (internal flash, always present) */
        if (stat("/fatfs", &st_check) == 0) {
            mp_obj_t root_fatfs = mp_obj_new_str_from_cstr("/fatfs");
            mp_obj_t vfs_fat = mp_call_function_1(
                MP_OBJ_FROM_PTR(&mp_type_vfs_posix), root_fatfs);
            mp_obj_t mount_args[2] = { vfs_fat, root_fatfs };
            mp_vfs_mount(2, mount_args, (mp_map_t *)&mp_const_empty_map);
            ESP_LOGI(TAG, "Mounted /fatfs in MicroPython VFS");
        }

        /* Mount /sdcard (SD card, optional) */
        if (stat("/sdcard", &st_check) == 0) {
            mp_obj_t root_sd = mp_obj_new_str_from_cstr("/sdcard");
            mp_obj_t vfs_sd = mp_call_function_1(
                MP_OBJ_FROM_PTR(&mp_type_vfs_posix), root_sd);
            mp_obj_t mount_args[2] = { vfs_sd, root_sd };
            mp_vfs_mount(2, mount_args, (mp_map_t *)&mp_const_empty_map);
            ESP_LOGI(TAG, "Mounted /sdcard in MicroPython VFS");
        }

        nlr_pop();
    } else {
        ESP_LOGW(TAG, "VFS mount failed (exception caught), continuing without");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "MicroPython runtime initialized (gc_heap=%d)", CAP_MPY_GC_HEAP_SIZE);
    return ESP_OK;
}

esp_err_t cap_mpy_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    mp_deinit();

    if (s_gc_heap) {
        heap_caps_free(s_gc_heap);
        s_gc_heap = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "MicroPython runtime deinitialized");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Synchronous script execution                                       */
/* ------------------------------------------------------------------ */

static esp_err_t cap_mpy_execute_file(const char *path,
                                      uint32_t timeout_ms,
                                      volatile bool *stop_requested,
                                      char *output,
                                      size_t output_size)
{
    struct stat st = {0};
    esp_err_t ret = ESP_OK;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    /* Validate path */
    if (!path || !path[0]) {
        snprintf(output, output_size, "Error: script path is empty");
        return ESP_ERR_INVALID_ARG;
    }

    size_t path_len = strlen(path);
    if (path_len <= 3 || strcmp(path + path_len - 3, ".py") != 0) {
        snprintf(output, output_size, "Error: path must be a .py script");
        return ESP_ERR_INVALID_ARG;
    }

    if (strstr(path, "..") != NULL) {
        snprintf(output, output_size, "Error: path must not contain '..'");
        return ESP_ERR_INVALID_ARG;
    }

    if (path[0] != '/') {
        snprintf(output, output_size, "Error: path must be absolute");
        return ESP_ERR_INVALID_ARG;
    }

    /* Check file exists and size */
    if (stat(path, &st) != 0) {
        snprintf(output, output_size, "Error: script not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0 || st.st_size > CAP_MPY_MAX_SCRIPT_SIZE) {
        snprintf(output, output_size, "Error: script size invalid: %ld bytes", (long)st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* NOTE: GC and runtime are already initialized by cap_mpy_init().
     * Do NOT re-init GC here — that would invalidate QSTR pool, builtins,
     * and other runtime objects allocated by mp_init(). */

    /* Read file via POSIX (ESP-IDF VFS), NOT MicroPython VFS.
     * MicroPython's VFS layer doesn't know about ESP-IDF mount points
     * like /sdcard or /fatfs, so we read the file ourselves and feed
     * the source string to the lexer. */
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(output, output_size, "Error: cannot open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t src_len = (size_t)st.st_size;
    char *src_buf = malloc(src_len + 1);
    if (!src_buf) {
        fclose(f);
        snprintf(output, output_size, "Error: out of memory (%u bytes)", (unsigned)src_len);
        return ESP_ERR_NO_MEM;
    }
    size_t nread = fread(src_buf, 1, src_len, f);
    fclose(f);
    src_buf[nread] = '\0';

    /* Compile and execute */
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        qstr source_name = qstr_from_str(path);

        /* Lex from string buffer (bypasses MicroPython VFS) */
        mp_lexer_t *lex = mp_lexer_new_from_str_len(source_name, src_buf, nread, false);
        if (lex == NULL) {
            snprintf(output, output_size, "Error: cannot lex file: %s", path);
            free(src_buf);
            nlr_pop();
            return ESP_ERR_NOT_FOUND;
        }

        mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);

        /* Compile to bytecode */
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, false);

        /* Execute */
        mp_call_function_0(module_fun);

        /* Success */
        free(src_buf);
        nlr_pop();
        if (output[0] == '\0') {
            snprintf(output, output_size, "Python script completed successfully.\n");
        }
    } else {
        free(src_buf);
        /* Exception occurred — format it into output buffer */
        mp_obj_t exc = (mp_obj_t)nlr.ret_val;
        vstr_t vstr;
        mp_print_t pr;
        vstr_init(&vstr, 256);
        pr = (mp_print_t){ &vstr, (mp_print_strn_t)vstr_add_strn };
        mp_obj_print_exception(&pr, exc);

        size_t msg_len = vstr.len;
        size_t copy_len = msg_len < output_size - 1 ? msg_len : output_size - 1;
        memcpy(output, vstr.buf, copy_len);
        output[copy_len] = '\0';
        vstr_clear(&vstr);

        ret = ESP_FAIL;
    }

    return ret;
}

esp_err_t cap_mpy_run_script(const char *path,
                             const char *args_json,
                             uint32_t timeout_ms,
                             char *output,
                             size_t output_size)
{
    (void)args_json; /* TODO: pass args to Python script */

    if (!s_initialized) {
        esp_err_t err = cap_mpy_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (timeout_ms == 0) {
        timeout_ms = CAP_MPY_DEFAULT_TIMEOUT_MS;
    }

    volatile bool stop_flag = false;
    return cap_mpy_execute_file(path, timeout_ms, &stop_flag, output, output_size);
}

bool cap_mpy_stop_requested(void)
{
    /* Phase 1: no global stop mechanism yet */
    return false;
}

/* ------------------------------------------------------------------ */
/*  Interactive REPL                                                    */
/* ------------------------------------------------------------------ */

void cap_mpy_repl(void)
{
    if (!s_initialized) {
        esp_err_t err = cap_mpy_init();
        if (err != ESP_OK) {
            printf("MicroPython runtime not available\n");
            return;
        }
    }

    /* Initialize event-driven REPL */
    pyexec_event_repl_init();

    /* Take over UART for direct character I/O */
    mp_hal_uart_open_repl();

    printf("\r\nMicroPython REPL ready. Ctrl+D to exit.\r\n");
    fflush(stdout);

    /* Feed UART characters to the REPL processor until exit.
     * Host terminal handles character echo.  We only echo \r\n and backspace
     * because the REPL processor does not produce these itself. */
    for (;;) {
        int c = mp_hal_stdin_rx_chr();

        if (c == '\r') {
            mp_hal_stdout_tx_strn("\r\n", 2);
        } else if (c == 0x7f || c == 0x08) {
            mp_hal_stdout_tx_strn("\x08 \x08", 3);
        }

        int ret = pyexec_event_repl_process_char(c);
        fflush(stdout); /* flush prompt / output immediately */
        if (ret & 0x100) { /* PYEXEC_FORCED_EXIT */
            break;
        }
    }

    /* Release UART so esp_console/linenoise can read again */
    mp_hal_uart_close();

    printf("\r\nExiting MicroPython REPL.\r\n");
}
