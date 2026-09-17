/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lua_tool.c
 * @brief File upload to SD card root via serial console.
 *
 * Upload protocol (MicroPython-style):
 *   fup <filename>              -- open file for writing
 *   fchunk <base64_data>        -- decode and append chunk
 *   fup done                    -- close file, print total size
 *
 * Run uploaded scripts with the existing ``lua`` command:
 *   lua --run --path /sdcard/main.lua
 *   lua --run-async --path /sdcard/main.lua
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_console.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "claw_paths.h"

/* cap_lua APIs are resolved at link time when the component is present.
 * Declared here to avoid a hard CMake dependency on cap_lua. */
#if CONFIG_APP_CLAW_CAP_LUA
esp_err_t cap_lua_stop_job(const char *id_or_name,
                           uint32_t wait_ms,
                           char *output,
                           size_t output_size);
esp_err_t cap_lua_stop_all_jobs(const char *exclusive_filter,
                                uint32_t wait_ms,
                                char *output,
                                size_t output_size);
#endif

static const char *TAG = "lua_tool";

/* ------------------------------------------------------------------ */
/*  File upload state                                                  */
/* ------------------------------------------------------------------ */

static FILE     *s_upload_file;
static size_t    s_upload_total;
static char      s_upload_path[192];

/* ------------------------------------------------------------------ */
/*  Filename validation                                                */
/* ------------------------------------------------------------------ */

static bool is_safe_filename(const char *name)
{
    if (!name || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  ``fup <filename>`` / ``fup done``                                  */
/* ------------------------------------------------------------------ */

static int cmd_fup(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  fup <filename>   -- start upload (file goes to data root)\n");
        printf("  fup done         -- finish upload, close file\n");
        if (s_upload_file) {
            printf("\nUpload in progress: %s (%u bytes so far)\n",
                   s_upload_path, (unsigned)s_upload_total);
        }
        return 0;
    }

    /* --- finish an in-progress upload --- */
    if (strcmp(argv[1], "done") == 0) {
        if (!s_upload_file) {
            printf("No upload in progress\n");
            return 1;
        }
        fclose(s_upload_file);
        s_upload_file = NULL;
        printf("Upload complete: %s (%u bytes)\n",
               s_upload_path, (unsigned)s_upload_total);
        ESP_LOGI(TAG, "Upload complete: %s (%u bytes)",
                 s_upload_path, (unsigned)s_upload_total);
        return 0;
    }

    /* --- start a new upload --- */
    const char *filename = argv[1];

    if (!is_safe_filename(filename)) {
        printf("Invalid filename: %s\n", filename);
        printf("Must be a simple name (no paths, no '..')\n");
        return 1;
    }

    if (s_upload_file) {
        printf("Upload already in progress for %s\n", s_upload_path);
        printf("Run 'fup done' to finish first\n");
        return 1;
    }

    /* Resolve writable root via claw_paths */
    char filepath[192];
    esp_err_t err = claw_paths_join(CLAW_PATH_DATA, filename,
                                    filepath, sizeof(filepath));
    if (err != ESP_OK) {
        printf("Path resolution failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    s_upload_file = fopen(filepath, "w");
    if (!s_upload_file) {
        printf("Cannot open %s for writing\n", filepath);
        return 1;
    }

    s_upload_total = 0;
    strlcpy(s_upload_path, filepath, sizeof(s_upload_path));
    printf("Ready: %s\n", filepath);
    printf("Send chunks with: fchunk <base64_data>\n");
    printf("Finish with:      fup done\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  ``fchunk <base64>``                                                */
/* ------------------------------------------------------------------ */

static int cmd_fchunk(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: fchunk <base64_data>\n");
        return 1;
    }
    if (!s_upload_file) {
        printf("No upload in progress. Start with: fup <filename>\n");
        return 1;
    }

    const char *b64 = argv[1];
    size_t b64_len = strlen(b64);

    if (b64_len == 0) {
        printf("Empty chunk, skipped\n");
        return 0;
    }

    /* Decode base64 */
    unsigned char *dec = malloc(b64_len);
    if (!dec) {
        printf("Out of memory\n");
        return 1;
    }

    size_t dec_len = 0;
    int ret = mbedtls_base64_decode(dec, b64_len, &dec_len,
                                    (const unsigned char *)b64, b64_len);
    if (ret != 0) {
        free(dec);
        printf("Base64 decode error (%d)\n", ret);
        return 1;
    }

    size_t written = fwrite(dec, 1, dec_len, s_upload_file);
    free(dec);

    if (written != dec_len) {
        printf("Write error (wrote %u of %u)\n",
               (unsigned)written, (unsigned)dec_len);
        return 1;
    }

    s_upload_total += dec_len;
    printf("OK %u bytes (total %u)\n", (unsigned)dec_len, (unsigned)s_upload_total);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  ``cp <src> <dst>``  — copy file                                    */
/* ------------------------------------------------------------------ */

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: cp <src> <dst>\n");
        printf("  Example: cp /fatfs/main.lua.example /sdcard/main.lua\n");
        return 1;
    }

    const char *src_path = argv[1];
    const char *dst_path = argv[2];

    FILE *src = fopen(src_path, "r");
    if (!src) {
        printf("Cannot open source: %s\n", src_path);
        return 1;
    }

    FILE *dst = fopen(dst_path, "w");
    if (!dst) {
        fclose(src);
        printf("Cannot open destination: %s\n", dst_path);
        return 1;
    }

    char buf[1024];
    size_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        size_t w = fwrite(buf, 1, n, dst);
        if (w != n) {
            printf("Write error at %u bytes\n", (unsigned)(total + w));
            fclose(src);
            fclose(dst);
            return 1;
        }
        total += n;
    }

    fclose(src);
    fclose(dst);
    printf("Copied %u bytes: %s -> %s\n", (unsigned)total, src_path, dst_path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  ``ls [dir]``  — list directory                                     */
/* ------------------------------------------------------------------ */

static int cmd_ls(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : "/sdcard";
    DIR *d = opendir(path);
    if (!d) {
        printf("Cannot open directory: %s\n", path);
        return 1;
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_type == DT_DIR) {
            printf("  <DIR>  %s\n", entry->d_name);
        } else {
            printf("  %s\n", entry->d_name);
        }
        count++;
    }
    closedir(d);
    printf("%d entries in %s\n", count, path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  ``luastop <id|name|all>``  — stop Lua async jobs                   */
/* ------------------------------------------------------------------ */

#if CONFIG_APP_CLAW_CAP_LUA
static int cmd_luastop(int argc, char **argv)
{
    char output[256] = {0};
    esp_err_t err;

    if (argc < 2) {
        printf("Usage:\n");
        printf("  luastop all        -- stop all async Lua jobs\n");
        printf("  luastop <id|name>  -- stop job by id or name\n");
        printf("  lua --jobs         -- list running jobs\n");
        return 0;
    }

    if (strcmp(argv[1], "all") == 0) {
        err = cap_lua_stop_all_jobs(NULL, 200, output, sizeof(output));
    } else {
        err = cap_lua_stop_job(argv[1], 200, output, sizeof(output));
    }

    if (output[0]) {
        printf("%s\n", output);
    }
    if (err == ESP_OK) {
        printf("OK\n");
    } else if (err == ESP_ERR_TIMEOUT) {
        printf("Stop requested (job may still be cleaning up)\n");
    } else {
        printf("Failed: %s\n", esp_err_to_name(err));
    }
    return (err == ESP_OK || err == ESP_ERR_TIMEOUT) ? 0 : 1;
}
#endif /* CONFIG_APP_CLAW_CAP_LUA */

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

esp_err_t lua_tool_register(void)
{
    esp_console_cmd_t fup_cmd = {
        .command = "fup",
        .help    = "File upload: fup <name> to start, fup done to finish",
        .func    = cmd_fup,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&fup_cmd));

    esp_console_cmd_t fchunk_cmd = {
        .command = "fchunk",
        .help    = "Send a base64 chunk: fchunk <base64_data>",
        .func    = cmd_fchunk,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&fchunk_cmd));

    esp_console_cmd_t cp_cmd = {
        .command = "cp",
        .help    = "Copy file: cp <src> <dst>",
        .func    = cmd_cp,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cp_cmd));

    esp_console_cmd_t ls_cmd = {
        .command = "ls",
        .help    = "List directory: ls [path]  (default: /sdcard)",
        .func    = cmd_ls,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ls_cmd));

#if CONFIG_APP_CLAW_CAP_LUA
    esp_console_cmd_t luastop_cmd = {
        .command = "luastop",
        .help    = "Stop Lua jobs: luastop all | luastop <id|name>",
        .func    = cmd_luastop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&luastop_cmd));
#endif

    ESP_LOGI(TAG, "File tool CLI ready (fup / fchunk / cp / ls / luastop)");
    return ESP_OK;
}
