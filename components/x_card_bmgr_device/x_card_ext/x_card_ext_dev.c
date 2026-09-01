/*
 * SPDX-FileCopyrightText: 2026 X-Card
 * SPDX-License-Identifier: MIT
 *
 * Custom external devices: servos, I2C ultrasonic, and I2C motor driver.
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_board_manager_includes.h"
#include "periph_ledc.h"
#include "x_card_ext_dev.h"

static const char *TAG = "x_ext_dev";

#define EXT_ULTRASONIC_I2C_TIMEOUT_MS 50
#define EXT_MOTOR_I2C_TIMEOUT_MS      100

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t       servo_id;
    const char *description;
    uint8_t      peripheral_count;
    const char *peripheral_name;
} x_card_ext_servo_config_t;

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    const char *description;
    uint8_t      peripheral_count;
    const char *peripheral_name;
} x_card_ext_other_config_t;

// Common config layout for both ext_servo_1 and ext_servo_2
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t       servo_id;
    const char * description;
    uint8_t     peripheral_count;
    const char *peripheral_name;
} ext_servo_config_common_t;

struct x_card_ext_servo_handle {
    periph_ledc_handle_t *ledc_periph;
    char *ledc_name;
    float current_angle;
};

struct x_card_ext_ultrasonic_handle {
    i2c_master_dev_handle_t i2c_dev;
};

struct x_card_ext_motor_handle {
    i2c_master_dev_handle_t i2c_dev;
};

/* -------------------------------------------------------------------
 *  Shared I2C helper functions (similar to x_card_sensors.c)
 * ------------------------------------------------------------------- */

static esp_err_t ext_i2c_get_bus(i2c_master_bus_handle_t *bus)
{
    void *raw = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle("i2c_master", &raw);
    if (ret != ESP_OK || !raw) {
        ESP_LOGE(TAG, "Failed to get i2c_master peripheral: %s", esp_err_to_name(ret));
        return ret != ESP_OK ? ret : ESP_ERR_NOT_FOUND;
    }
    *bus = (i2c_master_bus_handle_t)raw;
    return ESP_OK;
}

static esp_err_t ext_i2c_add_dev(i2c_master_bus_handle_t bus, uint8_t addr_7bit,
                                 i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_7bit,
        .scl_speed_hz    = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device at 0x%02X: %s", addr_7bit, esp_err_to_name(ret));
    }
    return ret;
}

/* ===================================================================
 *  External Servo Driver Implementation
 * =================================================================== */

int x_card_ext_servo_init(void *cfg, int cfg_size, void **device_handle)
{
    if (!cfg || !device_handle) {
        return -1;
    }
    const ext_servo_config_common_t *config = (const ext_servo_config_common_t *)cfg;

    if (!config->peripheral_name || strlen(config->peripheral_name) == 0) {
        ESP_LOGE(TAG, "No LEDC peripheral name configured for servo: %s", config->name);
        return -1;
    }

    periph_ledc_handle_t *ledc_periph = NULL;
    int ret = esp_board_periph_ref_handle(config->peripheral_name, (void **)&ledc_periph);
    if (ret != 0 || !ledc_periph) {
        ESP_LOGE(TAG, "Failed to get LEDC peripheral handle '%s': %d", config->peripheral_name, ret);
        return -1;
    }

    x_card_ext_servo_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        esp_board_periph_unref_handle(config->peripheral_name);
        return -1;
    }

    handle->ledc_periph = ledc_periph;
    handle->ledc_name = strdup(config->peripheral_name);
    handle->current_angle = 0.0f;
    *device_handle = handle;

    // Set initial duty representing 0 degrees
    x_card_ext_servo_set_angle(handle, 0.0f);

    ESP_LOGI(TAG, "ext_servo (id=%d) ready using peripheral %s (channel=%d)", 
             config->servo_id, config->peripheral_name, ledc_periph->channel);
    return 0;
}

int x_card_ext_servo_deinit(void *device_handle)
{
    if (!device_handle) return 0;
    x_card_ext_servo_handle_t *handle = (x_card_ext_servo_handle_t *)device_handle;

    x_card_ext_servo_stop(handle);

    if (handle->ledc_name) {
        esp_board_periph_unref_handle(handle->ledc_name);
        free(handle->ledc_name);
    }
    free(handle);
    ESP_LOGI(TAG, "ext_servo deinit");
    return 0;
}

esp_err_t x_card_ext_servo_stop(x_card_ext_servo_handle_t *handle)
{
    if (!handle || !handle->ledc_periph) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Keep the target angle in the handle so a later set_angle call can
     * resume the same LEDC channel without moving the servo to a default. */
    return ledc_stop(handle->ledc_periph->speed_mode,
                     handle->ledc_periph->channel, 0);
}

esp_err_t x_card_ext_servo_set_angle(x_card_ext_servo_handle_t *handle, float angle)
{
    if (!handle || !handle->ledc_periph) {
        return ESP_ERR_INVALID_ARG;
    }
    if (angle < 0.0f) angle = 0.0f;
    if (angle > 180.0f) angle = 180.0f;

    // Calculate target duty cycle (LEDC 14-bit resolution: 0 to 16383)
    // 0.5ms pulse (0°) -> 16384 * 0.5 / 20 = 409.6 (~410)
    // 2.5ms pulse (180°) -> 16384 * 2.5 / 20 = 2048
    // Duty = 410 + (angle / 180.0) * 1638
    uint32_t duty = (uint32_t)(410.0f + (angle / 180.0f) * 1638.0f);

    esp_err_t err = ledc_set_duty(handle->ledc_periph->speed_mode, handle->ledc_periph->channel, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_set_duty failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ledc_update_duty(handle->ledc_periph->speed_mode, handle->ledc_periph->channel);
    if (err == ESP_OK) {
        handle->current_angle = angle;
    } else {
        ESP_LOGE(TAG, "ledc_update_duty failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t x_card_ext_servo_get_angle(x_card_ext_servo_handle_t *handle, float *angle)
{
    if (!handle || !angle) {
        return ESP_ERR_INVALID_ARG;
    }
    *angle = handle->current_angle;
    return ESP_OK;
}

/* ===================================================================
 *  External I2C Ultrasonic Driver Implementation
 * =================================================================== */

int x_card_ext_ultrasonic_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg;
    (void)cfg_size;
    if (!device_handle) return -1;

    i2c_master_bus_handle_t bus;
    if (ext_i2c_get_bus(&bus) != ESP_OK) return -1;

    i2c_master_dev_handle_t dev;
    if (ext_i2c_add_dev(bus, 0x0B, &dev) != ESP_OK) return -1;

    x_card_ext_ultrasonic_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        i2c_master_bus_rm_device(dev);
        return -1;
    }

    handle->i2c_dev = dev;
    *device_handle = handle;

    ESP_LOGI(TAG, "ext_ultrasonic ready at address 0x0B");
    return 0;
}

int x_card_ext_ultrasonic_deinit(void *device_handle)
{
    if (!device_handle) return 0;
    x_card_ext_ultrasonic_handle_t *handle = (x_card_ext_ultrasonic_handle_t *)device_handle;
    i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    ESP_LOGI(TAG, "ext_ultrasonic deinit");
    return 0;
}

esp_err_t x_card_ext_ultrasonic_read_distance(x_card_ext_ultrasonic_handle_t *handle, float *distance)
{
    if (!handle || !distance) {
        return ESP_ERR_INVALID_ARG;
    }

    // Write command [0x01] to trigger ranging
    uint8_t cmd = 0x01;
    esp_err_t err = i2c_master_transmit(handle->i2c_dev, &cmd, 1,
                                        EXT_ULTRASONIC_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    // Wait 2ms for measurement
    vTaskDelay(pdMS_TO_TICKS(2));

    // Read 2 bytes distance data
    uint8_t raw[2] = {0};
    err = i2c_master_receive(handle->i2c_dev, raw, 2,
                             EXT_ULTRASONIC_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    // Calculate: distance = (raw[0] + raw[1] * 256) / 10.0
    float val = (float)(raw[0] + raw[1] * 256) / 10.0f;

    // Constrain distance to range [0, 200]
    if (val < 0.0f) val = 0.0f;
    if (val > 200.0f) val = 200.0f;

    *distance = val;
    return ESP_OK;
}

/* ===================================================================
 *  External I2C Dual Motor Driver Implementation
 * =================================================================== */

int x_card_ext_motor_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg;
    (void)cfg_size;
    if (!device_handle) return -1;

    i2c_master_bus_handle_t bus;
    if (ext_i2c_get_bus(&bus) != ESP_OK) return -1;

    i2c_master_dev_handle_t dev;
    if (ext_i2c_add_dev(bus, 0x11, &dev) != ESP_OK) return -1;

    x_card_ext_motor_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        i2c_master_bus_rm_device(dev);
        return -1;
    }

    handle->i2c_dev = dev;
    *device_handle = handle;

    ESP_LOGI(TAG, "ext_motor ready at address 0x11");
    return 0;
}

int x_card_ext_motor_deinit(void *device_handle)
{
    if (!device_handle) return 0;
    x_card_ext_motor_handle_t *handle = (x_card_ext_motor_handle_t *)device_handle;
    i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    ESP_LOGI(TAG, "ext_motor deinit");
    return 0;
}

esp_err_t x_card_ext_motor_set_speed(x_card_ext_motor_handle_t *handle, uint8_t motor_num, int8_t speed)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (motor_num != 1 && motor_num != 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (speed < -100) speed = -100;
    if (speed > 100) speed = 100;

    // Send 2 bytes: [motor_num, speed]
    uint8_t data[2] = { motor_num, (uint8_t)speed };
    esp_err_t err = i2c_master_transmit(handle->i2c_dev, data, sizeof(data),
                                        EXT_MOTOR_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ext_motor %u speed=%d I2C command failed: %s",
                 (unsigned int)motor_num, (int)speed, esp_err_to_name(err));
    }
    return err;
}

// Register devices to Board Manager
ESP_BOARD_ENTRY_IMPLEMENT(ext_servo_1, x_card_ext_servo_init, x_card_ext_servo_deinit);
ESP_BOARD_ENTRY_IMPLEMENT(ext_servo_2, x_card_ext_servo_init, x_card_ext_servo_deinit);
ESP_BOARD_ENTRY_IMPLEMENT(ext_ultrasonic, x_card_ext_ultrasonic_init, x_card_ext_ultrasonic_deinit);
ESP_BOARD_ENTRY_IMPLEMENT(ext_motor, x_card_ext_motor_init, x_card_ext_motor_deinit);
