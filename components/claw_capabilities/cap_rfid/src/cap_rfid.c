/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFID capability wrapping the abobija/rc522 library.
 * Provides LLM-callable tools: rfid_scan, rfid_stop, rfid_info.
 */
#include "cap_rfid.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_board_manager.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "rc522.h"
#include "rc522_types.h"
#include "driver/rc522_i2c.h"
#include "rc522_picc.h"

static const char *TAG = "cap_rfid";

#define CAP_RFID_I2C_ADDR           0x2F
#define CAP_RFID_I2C_SPEED_HZ      100000
#define CAP_RFID_DEFAULT_TIMEOUT_MS 3000
#define CAP_RFID_UID_STR_MAX        32

/* ------------------------------------------------------------------ */
/*  Module state                                                       */
/* ------------------------------------------------------------------ */

static struct {
    rc522_driver_handle_t driver;
    rc522_handle_t scanner;
    bool running;
    int detect_count;

    SemaphoreHandle_t card_sem;       /* Signalled on card detection */
    SemaphoreHandle_t mutex;          /* Protects shared state */

    /* Last detected card info (written by event handler, read by execute) */
    bool card_detected;
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;
    rc522_picc_type_t type;
} s_rfid = { 0 };

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static esp_err_t rfid_ensure_started(void);
static void rfid_shutdown(void);
static void rfid_on_picc_state_changed(void *arg, esp_event_base_t base,
                                        int32_t event_id, void *data);

/* ------------------------------------------------------------------ */
/*  Scanner lifecycle                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t rfid_ensure_started(void)
{
    if (s_rfid.running) {
        return ESP_OK;
    }

    esp_err_t err;

    /* Get board I2C bus */
    void *bus_raw = NULL;
    err = esp_board_manager_get_periph_handle("i2c_master", &bus_raw);
    ESP_RETURN_ON_ERROR(err, TAG, "I2C bus not available");
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bus_raw;

    /* Create RC522 I2C driver (reuses existing board bus) */
    rc522_i2c_config_t driver_config = {
        .bus = bus,
        .dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CAP_RFID_I2C_ADDR,
            .scl_speed_hz = CAP_RFID_I2C_SPEED_HZ,
        },
        .rst_io_num = GPIO_NUM_NC,
    };

    err = rc522_i2c_create(&driver_config, &s_rfid.driver);
    ESP_RETURN_ON_ERROR(err, TAG, "RC522 driver create failed");

    err = rc522_driver_install(s_rfid.driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RC522 driver install: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* Create scanner */
    rc522_config_t scanner_config = {
        .driver = s_rfid.driver,
    };
    err = rc522_create(&scanner_config, &s_rfid.scanner);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RC522 create: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* Register event handler */
    err = rc522_register_events(s_rfid.scanner, RC522_EVENT_PICC_STATE_CHANGED,
                                rfid_on_picc_state_changed, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Event registration: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* Start: reset → FIFO test → init (enables antenna) */
    err = rc522_start(s_rfid.scanner);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RC522 start: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* Create sync primitives */
    s_rfid.card_sem = xSemaphoreCreateBinary();
    s_rfid.mutex = xSemaphoreCreateMutex();
    if (!s_rfid.card_sem || !s_rfid.mutex) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    s_rfid.running = true;
    s_rfid.detect_count = 0;
    s_rfid.card_detected = false;

    ESP_LOGI(TAG, "RC522 scanner started @ 0x%02x", CAP_RFID_I2C_ADDR);
    return ESP_OK;

cleanup:
    rfid_shutdown();
    return err;
}

static void rfid_shutdown(void)
{
    if (s_rfid.scanner) {
        rc522_pause(s_rfid.scanner);
        rc522_unregister_events(s_rfid.scanner, RC522_EVENT_PICC_STATE_CHANGED,
                                rfid_on_picc_state_changed);
        rc522_destroy(s_rfid.scanner);
        s_rfid.scanner = NULL;
    }
    if (s_rfid.driver) {
        rc522_driver_uninstall(s_rfid.driver);
        s_rfid.driver = NULL;
    }
    if (s_rfid.card_sem) {
        vSemaphoreDelete(s_rfid.card_sem);
        s_rfid.card_sem = NULL;
    }
    if (s_rfid.mutex) {
        vSemaphoreDelete(s_rfid.mutex);
        s_rfid.mutex = NULL;
    }
    s_rfid.running = false;
    s_rfid.card_detected = false;
}

/* ------------------------------------------------------------------ */
/*  Event handler                                                      */
/* ------------------------------------------------------------------ */

static void rfid_on_picc_state_changed(void *arg, esp_event_base_t base,
                                        int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event =
        (rc522_picc_state_changed_event_t *)data;

    if (event->picc->state != RC522_PICC_STATE_ACTIVE) {
        return;
    }

    xSemaphoreTake(s_rfid.mutex, portMAX_DELAY);

    s_rfid.detect_count++;
    s_rfid.card_detected = true;
    s_rfid.uid_len = event->picc->uid.length;
    memcpy(s_rfid.uid, event->picc->uid.value, event->picc->uid.length);
    s_rfid.sak = event->picc->sak;
    s_rfid.type = event->picc->type;

    xSemaphoreGive(s_rfid.mutex);
    xSemaphoreGive(s_rfid.card_sem);

    ESP_LOGI(TAG, "Card detected (#%d)", s_rfid.detect_count);
}

/* ------------------------------------------------------------------ */
/*  Capability execute handlers                                        */
/* ------------------------------------------------------------------ */

static esp_err_t cap_rfid_execute_scan(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;

    int timeout_ms = CAP_RFID_DEFAULT_TIMEOUT_MS;

    if (input_json && input_json[0]) {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *item = cJSON_GetObjectItem(root, "timeout_ms");
            if (cJSON_IsNumber(item) && item->valueint > 0) {
                timeout_ms = item->valueint;
            }
            cJSON_Delete(root);
        }
    }

    esp_err_t err = rfid_ensure_started();
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"scanner init failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }

    /* Reset detection state before scanning */
    xSemaphoreTake(s_rfid.mutex, portMAX_DELAY);
    s_rfid.card_detected = false;
    xSemaphoreGive(s_rfid.mutex);

    /* Drain any stale semaphore count, then wait for card detection */
    xSemaphoreTake(s_rfid.card_sem, 0);
    bool detected = xSemaphoreTake(s_rfid.card_sem,
                                    pdMS_TO_TICKS(timeout_ms)) == pdTRUE;

    if (detected) {
        xSemaphoreTake(s_rfid.mutex, portMAX_DELAY);

        char uid_str[CAP_RFID_UID_STR_MAX] = {0};
        int pos = 0;
        for (int i = 0; i < s_rfid.uid_len && pos < (int)sizeof(uid_str) - 3; i++) {
            if (i > 0) {
                uid_str[pos++] = ':';
            }
            pos += snprintf(uid_str + pos, sizeof(uid_str) - pos, "%02X",
                            s_rfid.uid[i]);
        }

        const char *type_name = rc522_picc_type_name(s_rfid.type);

        snprintf(output, output_size,
                 "{\"ok\":true,\"detected\":true,"
                 "\"uid\":\"%s\",\"sak\":\"0x%02X\","
                 "\"type\":\"%s\",\"count\":%d}",
                 uid_str, s_rfid.sak, type_name, s_rfid.detect_count);

        xSemaphoreGive(s_rfid.mutex);
    } else {
        snprintf(output, output_size,
                 "{\"ok\":true,\"detected\":false,"
                 "\"message\":\"no card detected within %dms\"}",
                 timeout_ms);
    }

    return ESP_OK;
}

static esp_err_t cap_rfid_execute_stop(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    if (!s_rfid.running) {
        snprintf(output, output_size,
                 "{\"ok\":true,\"message\":\"scanner not active\"}");
        return ESP_OK;
    }

    rfid_shutdown();
    snprintf(output, output_size,
             "{\"ok\":true,\"message\":\"scanner stopped\"}");

    ESP_LOGI(TAG, "Scanner stopped by capability call");
    return ESP_OK;
}

static esp_err_t cap_rfid_execute_info(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    bool was_running = s_rfid.running;

    esp_err_t err = rfid_ensure_started();
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"scanner init failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size,
             "{\"ok\":true,\"i2c_addr\":\"0x%02X\","
             "\"i2c_speed_hz\":%d,\"running\":true,"
             "\"detect_count\":%d}",
             CAP_RFID_I2C_ADDR, CAP_RFID_I2C_SPEED_HZ,
             s_rfid.detect_count);

    /* If scanner was not previously running, shut it down to free resources */
    if (!was_running) {
        rfid_shutdown();
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Capability descriptors & group registration                        */
/* ------------------------------------------------------------------ */

static const claw_cap_descriptor_t s_rfid_descriptors[] = {
    {
        .id = "rfid_scan",
        .name = "rfid_scan",
        .family = "rfid",
        .description = "Scan for nearby RFID cards using the RC522 reader. "
                       "Blocks for the specified timeout and returns card UID, type, and SAK if detected.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                             "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":30000,"
                             "\"description\":\"Scan timeout in milliseconds (default 3000)\"}}}",
        .execute = cap_rfid_execute_scan,
    },
    {
        .id = "rfid_stop",
        .name = "rfid_stop",
        .family = "rfid",
        .description = "Stop the RFID scanner and release hardware resources.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rfid_execute_stop,
    },
    {
        .id = "rfid_info",
        .name = "rfid_info",
        .family = "rfid",
        .description = "Get RC522 RFID reader status and configuration information.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_rfid_execute_info,
    },
};

static const claw_cap_group_t s_rfid_group = {
    .group_id = "cap_rfid",
    .descriptors = s_rfid_descriptors,
    .descriptor_count = sizeof(s_rfid_descriptors) / sizeof(s_rfid_descriptors[0]),
};

esp_err_t cap_rfid_register_group(void)
{
    if (claw_cap_group_exists(s_rfid_group.group_id)) {
        return ESP_OK;
    }

    esp_err_t err = claw_cap_register_group(&s_rfid_group);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register group failed: %s", esp_err_to_name(err));
    }

    return err;
}
