/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_MPY_JOB_NAME_MAX            32
#define CAP_MPY_JOB_EXCLUSIVE_MAX       32
#define CAP_MPY_JOB_PATH_MAX            192
#define CAP_MPY_JOB_ID_LEN              9

typedef enum {
    CAP_MPY_JOB_QUEUED = 0,
    CAP_MPY_JOB_RUNNING,
    CAP_MPY_JOB_DONE,
    CAP_MPY_JOB_FAILED,
    CAP_MPY_JOB_TIMEOUT,
    CAP_MPY_JOB_STOPPED,
} cap_mpy_job_status_t;

typedef enum {
    CAP_MPY_JOB_EVENT_CREATED = 0,
    CAP_MPY_JOB_EVENT_RUNNING,
    CAP_MPY_JOB_EVENT_STOP_REQUESTED,
    CAP_MPY_JOB_EVENT_TERMINAL,
} cap_mpy_job_event_type_t;

typedef struct {
    cap_mpy_job_event_type_t type;
    cap_mpy_job_status_t status;
    char job_id[CAP_MPY_JOB_ID_LEN];
    char name[CAP_MPY_JOB_NAME_MAX];
    char exclusive[CAP_MPY_JOB_EXCLUSIVE_MAX];
    char path[CAP_MPY_JOB_PATH_MAX];
} cap_mpy_job_event_t;

typedef void (*cap_mpy_job_event_cb_t)(const cap_mpy_job_event_t *event, void *user_ctx);

/**
 * @brief Initialize the MicroPython runtime.
 * Must be called once before any other cap_mpy function.
 */
esp_err_t cap_mpy_init(void);

/**
 * @brief Deinitialize the MicroPython runtime and free all resources.
 */
esp_err_t cap_mpy_deinit(void);

/**
 * @brief Run a Python script synchronously.
 *
 * @param path      Absolute path to the .py script file
 * @param args_json Optional JSON string with script arguments (can be NULL)
 * @param timeout_ms Timeout in milliseconds (0 = default 60s)
 * @param output    Buffer to receive script output
 * @param output_size Size of output buffer
 * @return ESP_OK on success, ESP_FAIL on script error, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t cap_mpy_run_script(const char *path,
                             const char *args_json,
                             uint32_t timeout_ms,
                             char *output,
                             size_t output_size);

/**
 * @brief Run a Python script asynchronously with job management.
 *
 * @param path      Absolute path to the .py script file
 * @param args_json Optional JSON string with script arguments (can be NULL)
 * @param timeout_ms Timeout in milliseconds (0 = until cancelled)
 * @param name      Job name for identification (can be NULL)
 * @param exclusive Exclusive group name (can be NULL)
 * @param replace   If true, replace any conflicting job
 * @param output    Buffer to receive result message
 * @param output_size Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t cap_mpy_run_script_async(const char *path,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   const char *name,
                                   const char *exclusive,
                                   bool replace,
                                   char *output,
                                   size_t output_size);

/**
 * @brief Stop a specific async job by id or name.
 */
esp_err_t cap_mpy_stop_job(const char *id_or_name,
                           uint32_t wait_ms,
                           char *output,
                           size_t output_size);

/**
 * @brief Stop all async jobs, optionally filtered by exclusive group.
 */
esp_err_t cap_mpy_stop_all_jobs(const char *exclusive_filter,
                                uint32_t wait_ms,
                                char *output,
                                size_t output_size);

/**
 * @brief List all async jobs.
 */
esp_err_t cap_mpy_list_jobs(const char *status, char *output, size_t output_size);

/**
 * @brief Get the number of active (non-terminal) async jobs.
 */
size_t cap_mpy_get_active_job_count(void);

/**
 * @brief Register a callback for job lifecycle events.
 */
esp_err_t cap_mpy_register_job_event_cb(cap_mpy_job_event_cb_t cb, void *user_ctx);

/**
 * @brief Unregister a previously registered job event callback.
 */
esp_err_t cap_mpy_unregister_job_event_cb(cap_mpy_job_event_cb_t cb, void *user_ctx);

/**
 * @brief Check if the current MicroPython execution has been requested to stop.
 * Can be called from within a running script's C extension.
 */
bool cap_mpy_stop_requested(void);

/**
 * @brief Enter MicroPython interactive REPL mode.
 * Blocks until user exits (Ctrl+D or exit()).
 * Reads directly from UART, bypassing esp_console.
 */
void cap_mpy_repl(void);

#ifdef __cplusplus
}
#endif
