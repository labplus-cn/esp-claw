/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Production test for STM8S001 slave MCU.
 *
 * I2C address: 0x11
 * Register map:
 *   Reg 1 — Motor 1 speed (signed byte, -100..+100, negative = reverse)
 *   Reg 2 — Motor 2 speed (signed byte, -100..+100, negative = reverse)
 *   Reg 3 — Battery voltage (2 bytes, high byte first, unit mV)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_log.h"

static const char *TAG = "test_mcu";

#define STM8_I2C_ADDR          0x11
#define STM8_I2C_TIMEOUT_MS    100

#define STM8_REG_MOTOR1        1
#define STM8_REG_MOTOR2        2
#define STM8_REG_BATTERY       3

#define STM8_MOTOR_SPEED_MIN   (-100)
#define STM8_MOTOR_SPEED_MAX   100

/* ---- I2C helpers ---- */

static esp_err_t stm8_get_bus(i2c_master_dev_handle_t *out_dev)
{
    void *bus_raw = NULL;
    esp_err_t err = esp_board_manager_get_periph_handle("i2c_master", &bus_raw);
    if (err != ESP_OK) {
        printf("[FAIL] I2C bus 'i2c_master' not available: %s\n",
               esp_err_to_name(err));
        return err;
    }
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bus_raw;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = STM8_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, out_dev);
    if (err != ESP_OK) {
        printf("[FAIL] Cannot add I2C device 0x%02x: %s\n",
               STM8_I2C_ADDR, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t stm8_reg_write(i2c_master_dev_handle_t dev,
                                uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), STM8_I2C_TIMEOUT_MS);
}

static esp_err_t stm8_reg_read(i2c_master_dev_handle_t dev,
                               uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len,
                                       STM8_I2C_TIMEOUT_MS);
}

/* ---- Sub-commands ---- */

static int stm8_cmd_battery(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = stm8_get_bus(&dev);
    if (err != ESP_OK) {
        return 0;
    }

    uint8_t raw[2] = { 0 };
    /* Step 1: write register address (STOP released, STM8 prepares data) */
    uint8_t reg = STM8_REG_BATTERY;
    err = i2c_master_transmit(dev, &reg, 1, STM8_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("[FAIL] Battery reg write error: %s\n", esp_err_to_name(err));
        i2c_master_bus_rm_device(dev);
        return 0;
    }
    /* Step 2: read 2 bytes from the register */
    err = i2c_master_receive(dev, raw, 2, STM8_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("[FAIL] Battery read error: %s\n", esp_err_to_name(err));
        i2c_master_bus_rm_device(dev);
        return 0;
    }

    /* Low byte first → little-endian */
    uint16_t mv = ((uint16_t)raw[1] << 8) | raw[0];
    printf("Battery: %u mV  (raw: 0x%02x 0x%02x)\n", mv, raw[0], raw[1]);

    i2c_master_bus_rm_device(dev);
    return 0;
}

static int stm8_cmd_motor(int argc, char **argv, uint8_t reg, const char *name)
{
    if (argc < 2) {
        printf("Usage: test mcu %s <speed>\n", name);
        printf("  speed: %d to %d (negative = reverse, 0 = stop)\n",
               STM8_MOTOR_SPEED_MIN, STM8_MOTOR_SPEED_MAX);
        return 0;
    }

    int speed = atoi(argv[1]);
    if (speed < STM8_MOTOR_SPEED_MIN || speed > STM8_MOTOR_SPEED_MAX) {
        printf("[FAIL] Speed %d out of range [%d, %d]\n",
               speed, STM8_MOTOR_SPEED_MIN, STM8_MOTOR_SPEED_MAX);
        return 0;
    }

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = stm8_get_bus(&dev);
    if (err != ESP_OK) {
        return 0;
    }

    /* Convert signed speed to unsigned byte for I2C transmission */
    uint8_t val = (uint8_t)(int8_t)speed;
    err = stm8_reg_write(dev, reg, val);
    if (err != ESP_OK) {
        printf("[FAIL] %s write error: %s\n", name, esp_err_to_name(err));
        i2c_master_bus_rm_device(dev);
        return 0;
    }

    printf("%s: speed=%d (reg %d = 0x%02x)\n", name, speed, reg, val);

    i2c_master_bus_rm_device(dev);
    return 0;
}

static int stm8_cmd_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = stm8_get_bus(&dev);
    if (err != ESP_OK) {
        return 0;
    }

    /* Stop both motors */
    err = stm8_reg_write(dev, STM8_REG_MOTOR1, 0);
    if (err != ESP_OK) {
        printf("[FAIL] Motor1 stop error: %s\n", esp_err_to_name(err));
    }
    err = stm8_reg_write(dev, STM8_REG_MOTOR2, 0);
    if (err != ESP_OK) {
        printf("[FAIL] Motor2 stop error: %s\n", esp_err_to_name(err));
    }

    if (err == ESP_OK) {
        printf("Both motors stopped.\n");
    }

    i2c_master_bus_rm_device(dev);
    return 0;
}

/* ---- Main dispatcher ---- */

int test_mcu(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: test mcu <sub-command> [args...]\n");
        printf("Sub-commands:\n");
        printf("  battery           Read battery voltage (mV)\n");
        printf("  motor1 <speed>    Set motor 1 speed (-100..+100)\n");
        printf("  motor2 <speed>    Set motor 2 speed (-100..+100)\n");
        printf("  stop              Stop both motors\n");
        return 0;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "battery") == 0) {
        return stm8_cmd_battery(argc - 1, argv + 1);
    } else if (strcmp(sub, "motor1") == 0) {
        return stm8_cmd_motor(argc - 1, argv + 1, STM8_REG_MOTOR1, "motor1");
    } else if (strcmp(sub, "motor2") == 0) {
        return stm8_cmd_motor(argc - 1, argv + 1, STM8_REG_MOTOR2, "motor2");
    } else if (strcmp(sub, "stop") == 0) {
        return stm8_cmd_stop(argc - 1, argv + 1);
    }

    printf("Unknown mcu sub-command: '%s'\n", sub);
    return 0;
}
