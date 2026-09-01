/*
 * SPDX-FileCopyrightText: 2026 X-Card
 * SPDX-License-Identifier: MIT
 *
 * Custom external devices: servos, I2C ultrasonic, and I2C motor driver.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations of opaque device handle structures
typedef struct x_card_ext_servo_handle x_card_ext_servo_handle_t;
typedef struct x_card_ext_ultrasonic_handle x_card_ext_ultrasonic_handle_t;
typedef struct x_card_ext_motor_handle x_card_ext_motor_handle_t;

/* ===================================================================
 *  External Servo Device APIs
 * =================================================================== */

/**
 * @brief Set the angle of the external servo (0 to 180 degrees).
 */
esp_err_t x_card_ext_servo_set_angle(x_card_ext_servo_handle_t *handle, float angle);

/**
 * @brief Stop the servo PWM output without changing its last target angle.
 */
esp_err_t x_card_ext_servo_stop(x_card_ext_servo_handle_t *handle);

/**
 * @brief Get the current target angle of the external servo.
 */
esp_err_t x_card_ext_servo_get_angle(x_card_ext_servo_handle_t *handle, float *angle);

/* ===================================================================
 *  External I2C Ultrasonic Device APIs
 * =================================================================== */

/**
 * @brief Read distance in centimeters from the ultrasonic sensor.
 *        Returns distance in the range [0.0, 200.0] cm.
 */
esp_err_t x_card_ext_ultrasonic_read_distance(x_card_ext_ultrasonic_handle_t *handle, float *distance);

/* ===================================================================
 *  External I2C Dual Motor Device APIs
 * =================================================================== */

/**
 * @brief Set speed of the specified motor (-100 to 100).
 * @param motor_num Motor index (1 or 2).
 * @param speed Signed speed value (-100 to 100).
 */
esp_err_t x_card_ext_motor_set_speed(x_card_ext_motor_handle_t *handle, uint8_t motor_num, int8_t speed);

/* ===================================================================
 *  Board Manager Init/Deinit Entries
 * =================================================================== */
int x_card_ext_servo_init(void *cfg, int cfg_size, void **device_handle);
int x_card_ext_servo_deinit(void *device_handle);

int x_card_ext_ultrasonic_init(void *cfg, int cfg_size, void **device_handle);
int x_card_ext_ultrasonic_deinit(void *device_handle);

int x_card_ext_motor_init(void *cfg, int cfg_size, void **device_handle);
int x_card_ext_motor_deinit(void *device_handle);

#ifdef __cplusplus
}
#endif
