/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "hw_test_cli.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "dev_audio_codec.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"

static const char *TAG = "hw_test_audio";

#define AUDIO_TEST_SAMPLE_RATE   16000
#define AUDIO_TEST_BITS          16
#define AUDIO_TEST_CHANNELS      1
#define AUDIO_TEST_CHUNK         512
#define AUDIO_REC_DURATION_MS    3000
#define AUDIO_PLAY_DURATION_MS   3000   /* play back the full recording */
#define AUDIO_VOL                80
#define AUDIO_IN_GAIN_DB         24.0f
/* 3s @ 16kHz * 2 bytes = 96 KB — allocate from SPIRAM */
#define AUDIO_REC_BUF_SIZE       (AUDIO_TEST_SAMPLE_RATE * (AUDIO_REC_DURATION_MS / 1000) * sizeof(int16_t))

int test_audio(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ---- Get codec device handles ---- */
    dev_audio_codec_handles_t *dac_handles = NULL;
    dev_audio_codec_handles_t *adc_handles = NULL;

    esp_err_t err = esp_board_manager_get_device_handle("audio_dac", (void **)&dac_handles);
    if (err != ESP_OK || !dac_handles || !dac_handles->codec_dev) {
        printf("Audio DAC not available: %s\n", esp_err_to_name(err));
        return 0;
    }
    err = esp_board_manager_get_device_handle("audio_adc", (void **)&adc_handles);
    if (err != ESP_OK || !adc_handles || !adc_handles->codec_dev) {
        printf("Audio ADC not available: %s\n", esp_err_to_name(err));
        return 0;
    }

    esp_codec_dev_handle_t play_dev = dac_handles->codec_dev;
    esp_codec_dev_handle_t rec_dev  = adc_handles->codec_dev;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate    = AUDIO_TEST_SAMPLE_RATE,
        .channel        = AUDIO_TEST_CHANNELS,
        .bits_per_sample = AUDIO_TEST_BITS,
    };

    /* Recording buffer in SPIRAM */
    int16_t *rec_buf = heap_caps_malloc(AUDIO_REC_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rec_buf) {
        printf("Audio test: recording buffer alloc failed (%d bytes)\n", AUDIO_REC_BUF_SIZE);
        return 0;
    }

    int16_t *chunk_buf = heap_caps_malloc(AUDIO_TEST_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk_buf) {
        printf("Audio test: chunk buffer alloc failed\n");
        free(rec_buf);
        return 0;
    }

    int ret;
    int chunk_samp = AUDIO_TEST_CHUNK / (int)sizeof(int16_t);

    /* ================================================================
     * Phase 1: Record from mic into rec_buf
     * ================================================================ */
    printf("[1/2] Record %d ms from mic...\n", AUDIO_REC_DURATION_MS);
    ret = esp_codec_dev_open(rec_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        printf("  FAIL: open ADC (%d)\n", ret);
        goto cleanup;
    }
    esp_codec_dev_set_in_gain(rec_dev, AUDIO_IN_GAIN_DB);

    int rec_total = AUDIO_TEST_SAMPLE_RATE * (AUDIO_REC_DURATION_MS / 1000);
    int rec_done  = 0;
    int16_t peak = 0, trough = 0;

    while (rec_done < rec_total) {
        int n = (rec_total - rec_done > chunk_samp) ? chunk_samp : (rec_total - rec_done);
        memset(chunk_buf, 0, n * sizeof(int16_t));
        ret = esp_codec_dev_read(rec_dev, chunk_buf, n * sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) {
            printf("  FAIL: ADC read (%d) at sample %d\n", ret, rec_done);
            esp_codec_dev_close(rec_dev);
            goto cleanup;
        }
        /* Store into recording buffer */
        memcpy(&rec_buf[rec_done], chunk_buf, n * sizeof(int16_t));
        /* Track signal level */
        for (int i = 0; i < n; i++) {
            if (chunk_buf[i] > peak)   peak   = chunk_buf[i];
            if (chunk_buf[i] < trough) trough = chunk_buf[i];
        }
        rec_done += n;
    }
    esp_codec_dev_close(rec_dev);

    int amplitude = (int)(peak - trough);
    printf("  Recorded %d samples, peak=%d trough=%d amplitude=%d\n",
           rec_done, peak, trough, amplitude);
    if (amplitude < 200) {
        printf("  WARN: Mic signal very weak (may be silent)\n");
    } else {
        printf("  Mic signal detected\n");
    }

    /* ================================================================
     * Phase 2: Play back the recorded audio through speaker
     * ================================================================ */
    printf("[2/2] Playing back recorded audio (%d ms)...\n", AUDIO_REC_DURATION_MS);
    ret = esp_codec_dev_open(play_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        printf("  FAIL: open DAC (%d)\n", ret);
        goto cleanup;
    }
    esp_codec_dev_set_out_vol(play_dev, AUDIO_VOL);

    int play_total = rec_done; /* play exactly what was recorded */
    int play_done  = 0;

    while (play_done < play_total) {
        int n = (play_total - play_done > chunk_samp) ? chunk_samp : (play_total - play_done);
        ret = esp_codec_dev_write(play_dev, &rec_buf[play_done], n * sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) {
            printf("  FAIL: DAC write (%d) at sample %d\n", ret, play_done);
            esp_codec_dev_close(play_dev);
            goto cleanup;
        }
        play_done += n;
    }
    esp_codec_dev_close(play_dev);
    printf("  Playback done (%d samples)\n", play_done);

cleanup:
    free(rec_buf);
    free(chunk_buf);
    printf("Audio test complete.\n");
    return 0;
}
