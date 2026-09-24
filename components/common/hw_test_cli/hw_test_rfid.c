/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Production test for RC522 RFID reader (I2C interface).
 * Uses abobija/rc522 library for all chip communication.
 *
 * Commands:
 *   test rfid              -- one-shot: init chip, scan 3s, report
 *   test rfid start        -- continuous scanning with card detection events
 *   test rfid stop         -- stop scanning, release resources
 *   test rfid --addr 0x2F  -- override I2C address
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_board_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rc522.h"
#include "rc522_types.h"
#include "driver/rc522_i2c.h"
#include "rc522_picc.h"

static const char *TAG = "test_rfid";

#define RC522_I2C_ADDR       0x2F
#define RC522_I2C_SPEED_HZ   100000

/* ------------------------------------------------------------------ */
/*  Persistent state                                                    */
/* ------------------------------------------------------------------ */

static struct {
    rc522_driver_handle_t driver;
    rc522_handle_t scanner;
    bool running;
    uint8_t i2c_addr;
    int detect_count;
} s_rfid = { 0 };

/* ------------------------------------------------------------------ */
/*  Event handler for card detection                                    */
/* ------------------------------------------------------------------ */

static void rfid_on_picc_state_changed(void *arg, esp_event_base_t base,
                                        int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event =
        (rc522_picc_state_changed_event_t *)data;

    if (event->picc->state != RC522_PICC_STATE_ACTIVE) {
        return;
    }

    s_rfid.detect_count++;

    rc522_picc_uid_t uid = event->picc->uid;
    printf("  [RFID] Card detected! #%d  Type: %s  UID: ",
           s_rfid.detect_count, rc522_picc_type_name(event->picc->type));
    for (int i = 0; i < uid.length; i++) {
        printf("%02X", uid.value[i]);
        if (i < uid.length - 1) {
            printf(":");
        }
    }
    printf("  SAK: 0x%02x\n", event->picc->sak);
}

/* ------------------------------------------------------------------ */
/*  Cleanup helper                                                      */
/* ------------------------------------------------------------------ */

static void rfid_cleanup(void)
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
    s_rfid.running = false;
}

/* ------------------------------------------------------------------ */
/*  Create driver + scanner using library API                           */
/* ------------------------------------------------------------------ */

static esp_err_t rfid_create_and_start(uint8_t i2c_addr)
{
    esp_err_t err;

    /* Get existing I2C bus from board manager */
    void *bus_raw = NULL;
    err = esp_board_manager_get_periph_handle("i2c_master", &bus_raw);
    if (err != ESP_OK) {
        printf("[FAIL] I2C bus not available: %s\n", esp_err_to_name(err));
        return err;
    }
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bus_raw;

    /* Create RC522 I2C driver (reuses existing bus, no RST pin) */
    rc522_i2c_config_t driver_config = {
        .bus = bus,
        .dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = i2c_addr,
            .scl_speed_hz = RC522_I2C_SPEED_HZ,
        },
        .rst_io_num = GPIO_NUM_NC,
    };

    err = rc522_i2c_create(&driver_config, &s_rfid.driver);
    if (err != ESP_OK) {
        printf("[FAIL] RC522 driver create: %s\n", esp_err_to_name(err));
        return err;
    }

    err = rc522_driver_install(s_rfid.driver);
    if (err != ESP_OK) {
        printf("[FAIL] RC522 driver install: %s\n", esp_err_to_name(err));
        return err;
    }

    /* Create scanner (allocates resources, creates polling task) */
    rc522_config_t scanner_config = {
        .driver = s_rfid.driver,
    };

    err = rc522_create(&scanner_config, &s_rfid.scanner);
    if (err != ESP_OK) {
        printf("[FAIL] RC522 create: %s\n", esp_err_to_name(err));
        return err;
    }

    /* Register event handler */
    err = rc522_register_events(s_rfid.scanner, RC522_EVENT_PICC_STATE_CHANGED,
                                rfid_on_picc_state_changed, NULL);
    if (err != ESP_OK) {
        printf("[FAIL] Event registration: %s\n", esp_err_to_name(err));
        return err;
    }

    /*
     * rc522_start internally does:
     * 1. rc522_pcd_reset() - soft reset (since no RST pin)
     * 2. rc522_pcd_rw_test() - FIFO read/write test
     * 3. rc522_pcd_init()  - configure all registers + enable antenna:
     *    - Reset TxModeReg, RxModeReg, ModWidthReg
     *    - Timer: TAuto=1, prescaler=169, reload=1000
     *    - 100%% ASK modulation
     *    - ModeReg = 0x3D (CRC preset 0x6363)
     *    - TxControlReg = 0x03 (antenna ON)
     */
    err = rc522_start(s_rfid.scanner);
    if (err != ESP_OK) {
        printf("[FAIL] RC522 start: %s\n", esp_err_to_name(err));
        return err;
    }

    s_rfid.i2c_addr = i2c_addr;
    s_rfid.running = true;
    s_rfid.detect_count = 0;

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  test rfid start                                                     */
/* ------------------------------------------------------------------ */

static int rfid_start(int argc, char **argv)
{
    uint8_t i2c_addr = RC522_I2C_ADDR;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            i2c_addr = (uint8_t)strtol(argv[++i], NULL, 0);
        }
    }

    if (s_rfid.running) {
        printf("RFID already running. Use 'test rfid stop' first.\n");
        return 0;
    }

    esp_err_t err = rfid_create_and_start(i2c_addr);
    if (err != ESP_OK) {
        rfid_cleanup();
        return 0;
    }

    printf("RFID scanning ON @ 0x%02x. Bring card close to antenna.\n", i2c_addr);
    printf("Use 'test rfid stop' to disable.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  test rfid stop                                                      */
/* ------------------------------------------------------------------ */

static int rfid_stop(void)
{
    if (!s_rfid.running) {
        printf("RFID is not active.\n");
        return 0;
    }

    rfid_cleanup();
    printf("RFID stopped.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  test rfid  (one-shot or dispatch to start/stop)                     */
/* ------------------------------------------------------------------ */

int test_rfid(int argc, char **argv)
{
    /* Check for start/stop sub-commands */
    if (argc >= 2) {
        if (strcmp(argv[1], "start") == 0) {
            return rfid_start(argc, argv);
        }
        if (strcmp(argv[1], "stop") == 0) {
            return rfid_stop();
        }
    }

    /* Default: one-shot diagnostic test */
    uint8_t i2c_addr = RC522_I2C_ADDR;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            i2c_addr = (uint8_t)strtol(argv[++i], NULL, 0);
        }
    }

    printf("RFID test: RC522 @ 0x%02x\n", i2c_addr);

    if (s_rfid.running) {
        printf("[INFO] RFID already running, stop it first.\n");
        return 0;
    }

    /* Create and start scanner (library handles all chip init) */
    printf("  Initializing via library...\n");
    esp_err_t err = rfid_create_and_start(i2c_addr);
    if (err != ESP_OK) {
        rfid_cleanup();
        printf("\n=== RFID test FAILED ===\n");
        return 0;
    }
    printf("  Chip initialized, antenna enabled, scanning started.\n");

    /* Wait 3 seconds for card detection */
    printf("  Waiting 3s for card...\n");
    vTaskDelay(pdMS_TO_TICKS(3000));

    bool pass = false;
    if (s_rfid.detect_count > 0) {
        printf("  Card detected during test window!\n");
        pass = true;
    } else {
        printf("  No card in range (chip is functional)\n");
        pass = true;  /* Chip init succeeded, so it's functional */
    }

    /* Cleanup */
    rfid_cleanup();

    if (pass) {
        printf("\n=== RFID test PASSED (RC522 @ 0x%02x) ===\n", i2c_addr);
    } else {
        printf("\n=== RFID test FAILED ===\n");
    }

    return 0;
}
