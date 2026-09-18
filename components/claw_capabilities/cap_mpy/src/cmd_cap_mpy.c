/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CLI commands for the MicroPython capability (cap_mpy).
 */
#include "cmd_cap_mpy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cap_mpy.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "cmd_cap_mpy";

static struct {
    struct arg_lit *run;
    struct arg_lit *run_async;
    struct arg_lit *jobs;
    struct arg_lit *repl;
    struct arg_str *stop;
    struct arg_str *path;
    struct arg_str *name;
    struct arg_str *exclusive;
    struct arg_str *status;
    struct arg_int *timeout_ms;
    struct arg_end *end;
} mpy_args;

#define MPY_OUTPUT_SIZE (4 * 1024)

static int mpy_func(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&mpy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, mpy_args.end, argv[0]);
        return 1;
    }

    int operation_count = mpy_args.run->count + mpy_args.run_async->count +
                          mpy_args.jobs->count + mpy_args.stop->count +
                          mpy_args.repl->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    char *output = calloc(1, MPY_OUTPUT_SIZE);
    if (!output) {
        printf("failed to allocate output buffer\n");
        return 1;
    }

    esp_err_t err = ESP_OK;
    uint32_t timeout_ms = 0;

    if (mpy_args.timeout_ms->count) {
        if (mpy_args.timeout_ms->ival[0] <= 0) {
            printf("'--timeout-ms' must be a positive integer\n");
            free(output);
            return 1;
        }
        timeout_ms = (uint32_t)mpy_args.timeout_ms->ival[0];
    }

    if (mpy_args.run->count) {
        if (!mpy_args.path->count) {
            printf("'--run' requires '--path'\n");
            free(output);
            return 1;
        }
        err = cap_mpy_run_script(mpy_args.path->sval[0], NULL, timeout_ms, output, MPY_OUTPUT_SIZE);
        if (err != ESP_OK) {
            printf("%s", output);
            printf("mpy command failed: %s\n", esp_err_to_name(err));
            free(output);
            return 1;
        }
        printf("%s", output);
    } else if (mpy_args.run_async->count) {
        if (!mpy_args.path->count) {
            printf("'--run-async' requires '--path'\n");
            free(output);
            return 1;
        }
        const char *job_name = mpy_args.name->count ? mpy_args.name->sval[0] : NULL;
        const char *excl = mpy_args.exclusive->count ? mpy_args.exclusive->sval[0] : NULL;
        err = cap_mpy_run_script_async(mpy_args.path->sval[0], NULL, timeout_ms,
                                       job_name, excl, false, output, MPY_OUTPUT_SIZE);
        if (err != ESP_OK) {
            printf("%s", output);
            printf("mpy command failed: %s\n", esp_err_to_name(err));
            free(output);
            return 1;
        }
        printf("%s\n", output);
    } else if (mpy_args.jobs->count) {
        const char *status_filter = mpy_args.status->count ? mpy_args.status->sval[0] : NULL;
        err = cap_mpy_list_jobs(status_filter, output, MPY_OUTPUT_SIZE);
        if (err != ESP_OK) {
            printf("mpy command failed: %s\n", esp_err_to_name(err));
            free(output);
            return 1;
        }
        printf("%s", output);
    } else if (mpy_args.stop->count) {
        err = cap_mpy_stop_job(mpy_args.stop->sval[0], 2000, output, MPY_OUTPUT_SIZE);
        if (err != ESP_OK) {
            printf("%s", output);
            printf("mpy command failed: %s\n", esp_err_to_name(err));
            free(output);
            return 1;
        }
        printf("%s\n", output);
    } else if (mpy_args.repl->count) {
        free(output);
        cap_mpy_repl();
        return 0;
    }

    free(output);
    return 0;
}

static int mpystop_func(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: mpystop <job_id|name|all>\n");
        return 1;
    }

    char *output = calloc(1, MPY_OUTPUT_SIZE);
    if (!output) {
        printf("failed to allocate output buffer\n");
        return 1;
    }

    esp_err_t err;
    if (strcmp(argv[1], "all") == 0) {
        err = cap_mpy_stop_all_jobs(NULL, 2000, output, MPY_OUTPUT_SIZE);
    } else {
        err = cap_mpy_stop_job(argv[1], 2000, output, MPY_OUTPUT_SIZE);
    }

    if (err != ESP_OK) {
        printf("%s", output);
        printf("mpystop failed: %s\n", esp_err_to_name(err));
        free(output);
        return 1;
    }

    printf("%s\n", output);
    free(output);
    return 0;
}

void register_cap_mpy(void)
{
    /* mpy command */
    mpy_args.run = arg_lit0("r", "run", "Run a Python script synchronously");
    mpy_args.run_async = arg_lit0(NULL, "run-async", "Run a Python script asynchronously");
    mpy_args.jobs = arg_lit0("j", "jobs", "List async Python jobs");
    mpy_args.repl = arg_lit0(NULL, "repl", "Enter MicroPython interactive REPL");
    mpy_args.stop = arg_str0(NULL, "stop", "<id>", "Stop an async Python job");
    mpy_args.path = arg_str0("p", "path", "<path>", "Absolute .py file path");
    mpy_args.name = arg_str0("n", "name", "<name>", "Job name for identification");
    mpy_args.exclusive = arg_str0("e", "exclusive", "<group>", "Exclusive group name");
    mpy_args.status = arg_str0(NULL, "status", "<status>", "Job status filter: all|queued|running|done|failed|timeout|stopped");
    mpy_args.timeout_ms = arg_int0("t", "timeout-ms", "<ms>", "Execution timeout in milliseconds");
    mpy_args.end = arg_end(10);

    const esp_console_cmd_t mpy_cmd = {
        .command = "mpy",
        .help = "MicroPython script operations.\n"
        "Examples:\n"
        " mpy --run --path /fatfs/test.py\n"
        " mpy --run-async --path /fatfs/main.py --name myjob\n"
        " mpy --jobs --status running\n"
        " mpy --stop <job_id>\n"
        " mpy --repl\n",
        .func = mpy_func,
        .argtable = &mpy_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mpy_cmd));

    /* mpystop command */
    const esp_console_cmd_t mpystop_cmd = {
        .command = "mpystop",
        .help = "Stop MicroPython async jobs.\n"
        "Usage: mpystop all | mpystop <job_id>\n",
        .func = mpystop_func,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mpystop_cmd));

    ESP_LOGI(TAG, "MicroPython CLI commands registered");
}
