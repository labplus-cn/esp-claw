/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "hw_test_cli.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_board_manager.h"
#include "driver/i2c_master.h"
#include "esp_vfs_fat.h"
#include "esp_err.h"

static const char *TAG = "hw_test";

/* ------------------------------------------------------------------ */
/*  Sub-command function prototypes (implemented in separate files)    */
/* ------------------------------------------------------------------ */

/* hw_test_cli.c — I2C bus scan */
int test_i2c(int argc, char **argv);

/* hw_test_cli.c — SD card write / read / verify */
int test_sdcard(int argc, char **argv);

/* hw_test_display.c — draw color blocks on LCD */
int test_screen(int argc, char **argv);

/* hw_test_camera.c — camera preview on LCD */
int test_camera(int argc, char **argv);

/* hw_test_audio.c — audio playback / record / echo */
int test_audio(int argc, char **argv);

/* hw_test_touch.c — touch panel crosshair test */
int test_touch(int argc, char **argv);

/* hw_test_lvgl.c — LVGL widget touch response test */
int test_lvgl(int argc, char **argv);

/* lua_tool.c — file upload via serial console */
esp_err_t lua_tool_register(void);

/* ------------------------------------------------------------------ */
/*  Sub-command dispatch table                                         */
/*  To add a new test:                                                 */
/*    1. Implement  int test_xxx(int argc, char **argv)                */
/*    2. Add a row here.                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *help;
    int (*handler)(int argc, char **argv);
} hw_test_subcmd_t;

static const hw_test_subcmd_t s_sub_commands[] = {
    { "i2c",     "Scan I2C bus for devices",                test_i2c     },
    { "sdcard",  "SD card write / read / verify",           test_sdcard  },
    { "screen",  "Draw RGB color blocks on LCD",            test_screen  },
    { "camera",  "Camera preview on LCD (arg: stop)",       test_camera  },
    { "audio",   "Record mic then play back through speaker", test_audio   },
    { "touch",   "Touch panel test (arg: stop)",              test_touch   },
    { "lvgl",    "LVGL widget touch test (arg: stop)",        test_lvgl    },
};

/* ------------------------------------------------------------------ */
/*  ``test`` dispatcher                                                */
/* ------------------------------------------------------------------ */

static int cmd_test(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: test <sub-command> [args...]\n");
        printf("Available sub-commands:\n");
        for (size_t i = 0; i < sizeof(s_sub_commands) / sizeof(s_sub_commands[0]); i++) {
            printf("  %-10s %s\n", s_sub_commands[i].name, s_sub_commands[i].help);
        }
        return 0;
    }

    const char *sub = argv[1];

    for (size_t i = 0; i < sizeof(s_sub_commands) / sizeof(s_sub_commands[0]); i++) {
        if (strcmp(sub, s_sub_commands[i].name) == 0) {
            /* Shift argv so the sub-command handler sees its own name at argv[0] */
            return s_sub_commands[i].handler(argc - 1, argv + 1);
        }
    }

    printf("Unknown test sub-command: '%s'\n", sub);
    printf("Run 'test' with no arguments to list available sub-commands.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  test i2c — scan I2C bus                                           */
/* ------------------------------------------------------------------ */

int test_i2c(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    void *handle = NULL;
    esp_err_t err = esp_board_manager_get_periph_handle("i2c_master", &handle);
    if (err != ESP_OK) {
        printf("I2C bus not available: %s\n", esp_err_to_name(err));
        return 0;
    }
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)handle;

    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    int found = 0;
    for (uint8_t addr = 0; addr < 128; addr++) {
        if ((addr & 0x0F) == 0) {
            printf("%02x: ", addr);
        }
        if (addr < 0x03 || addr > 0x77) {
            printf("   ");
        } else if (i2c_master_probe(bus, addr, 100) == ESP_OK) {
            printf("%02x ", addr);
            found++;
        } else {
            printf("-- ");
        }
        if ((addr & 0x0F) == 0x0F) {
            printf("\n");
        }
    }
    printf("Scan done. %d device(s) found.\n", found);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  test sdcard — write / read / verify                                */
/* ------------------------------------------------------------------ */

static const char *SDCARD_TEST_DATA = "ESP-Claw hardware test: SD card OK\n";

int test_sdcard(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Try writable paths: SD card first, then flash fatfs */
    const char *test_paths[] = { "/sdcard/_hw_test.tmp", "/fatfs/_hw_test.tmp" };
    const char *test_path = NULL;

    for (int i = 0; i < sizeof(test_paths) / sizeof(test_paths[0]); i++) {
        FILE *probe = fopen(test_paths[i], "w");
        if (probe) {
            fclose(probe);
            remove(test_paths[i]);
            test_path = test_paths[i];
            break;
        }
    }
    if (!test_path) {
        printf("SD write FAILED: no writable path found (tried /sdcard, /fatfs)\n");
        return 0;
    }
    /* Write */
    FILE *f = fopen(test_path, "w");
    if (!f) {
        printf("SD write FAILED: cannot open %s\n", test_path);
        return 0;
    }
    size_t written = fwrite(SDCARD_TEST_DATA, 1, strlen(SDCARD_TEST_DATA), f);
    fclose(f);
    printf("Write: %zu bytes -> %s\n", written, test_path);

    /* Read back */
    f = fopen(test_path, "r");
    if (!f) {
        printf("SD read FAILED: cannot reopen %s\n", test_path);
        return 0;
    }
    char buf[128] = {0};
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    /* Verify */
    size_t expected = strlen(SDCARD_TEST_DATA);
    if (rd == expected && memcmp(buf, SDCARD_TEST_DATA, expected) == 0) {
        printf("Read+verify: OK (%.*s)\n", (int)rd, buf);
    } else {
        printf("Verify FAILED: expected %zu bytes, got %zu\n", expected, rd);
        if (rd > 0) {
            printf("  content: %.*s\n", (int)rd, buf);
        }
    }

    /* Cleanup */
    remove(test_path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public init — register the ``test`` command                        */
/* ------------------------------------------------------------------ */

esp_err_t hw_test_cli_init(void)
{
    esp_console_cmd_t cmd = {
        .command = "test",
        .help = "Hardware test: test <i2c|sdcard|screen|camera|audio|touch|lvgl> [args...]",
        .func = cmd_test,
    };
    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register 'test' command: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Hardware test CLI ready (%d sub-commands)",
             (int)(sizeof(s_sub_commands) / sizeof(s_sub_commands[0])));

    /* Register file-upload commands (fup / fchunk) */
    lua_tool_register();

    return ESP_OK;
}
