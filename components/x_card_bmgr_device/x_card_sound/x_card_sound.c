#include <stdlib.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_board_manager_includes.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "periph_adc.h"
#include "x_card_sound.h"

static const char *TAG = "x_sound_adc";

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t     samples;
    const char *description;
    uint8_t    peripheral_count;
    const char *peripheral_name;
} x_card_sound_adc_config_t;

#define X_CARD_SOUND_ADC_PERIPH_NAME  "adc_battery" // Share the unit from battery ADC
#define X_CARD_SOUND_ADC_CHANNEL      ADC_CHANNEL_5
#define X_CARD_SOUND_ADC_SAMPLES      4U

struct x_card_sound_adc_handle {
    periph_adc_handle_t *adc_periph;
    adc_channel_t        channel;
    adc_cali_handle_t    cali_handle;
    uint8_t              sample_count;
    bool                 cali_enabled;
};

int x_card_sound_adc_init(void *cfg, int cfg_size, void **device_handle)
{
    if (!device_handle) {
        return -1;
    }

    periph_adc_handle_t *adc_periph = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle(X_CARD_SOUND_ADC_PERIPH_NAME,
                                                        (void **)&adc_periph);
    if (ret != ESP_OK || !adc_periph || !adc_periph->oneshot) {
        ESP_LOGE(TAG, "get %s peripheral failed: %s",
                 X_CARD_SOUND_ADC_PERIPH_NAME, esp_err_to_name(ret));
        return -1;
    }

    x_card_sound_adc_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return -1;
    }

    handle->adc_periph = adc_periph;
    handle->channel = X_CARD_SOUND_ADC_CHANNEL;
    handle->sample_count = X_CARD_SOUND_ADC_SAMPLES;

    if (cfg && cfg_size >= (int)sizeof(x_card_sound_adc_config_t)) {
        const x_card_sound_adc_config_t *sound_cfg = cfg;
        if (sound_cfg->samples > 0) {
            handle->sample_count = (uint8_t)sound_cfg->samples;
        }
    }

    // Configure channel 5 on the shared oneshot unit
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ret = adc_oneshot_config_channel(adc_periph->oneshot, handle->channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Configure channel 5 failed: %s", esp_err_to_name(ret));
        free(handle);
        return -1;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = handle->channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &handle->cali_handle);
    if (ret == ESP_OK) {
        handle->cali_enabled = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable: %s", esp_err_to_name(ret));
    }

    *device_handle = handle;
    ESP_LOGI(TAG, "sound_adc ready on %s channel %d, samples=%u, calibrated=%s",
             X_CARD_SOUND_ADC_PERIPH_NAME, handle->channel,
             handle->sample_count, handle->cali_enabled ? "yes" : "no");
    return 0;
}

int x_card_sound_adc_deinit(void *device_handle)
{
    x_card_sound_adc_handle_t *handle = device_handle;
    if (handle && handle->cali_enabled && handle->cali_handle) {
        adc_cali_delete_scheme_curve_fitting(handle->cali_handle);
    }
    free(handle);
    ESP_LOGI(TAG, "sound_adc deinit");
    return 0;
}

esp_err_t x_card_sound_adc_read_raw(x_card_sound_adc_handle_t *handle, int *raw)
{
    if (!handle || !raw || !handle->adc_periph || !handle->adc_periph->oneshot) {
        return ESP_ERR_INVALID_ARG;
    }

    int max_val = 0;
    int min_val = 4095;
    
    // Sample over a 20ms window to catch frequencies down to 50Hz envelope
    int64_t start_time = esp_timer_get_time();
    while ((esp_timer_get_time() - start_time) < 20000) {
        int sample = 0;
        esp_err_t ret = adc_oneshot_read(handle->adc_periph->oneshot, handle->channel, &sample);
        if (ret == ESP_OK) {
            if (sample > max_val) max_val = sample;
            if (sample < min_val) min_val = sample;
        } else {
            return ret; // Abort on error
        }
    }

    int amplitude = max_val - min_val;
    if (amplitude < 0) amplitude = 0;
    
    *raw = amplitude;
    return ESP_OK;
}

esp_err_t x_card_sound_adc_read_mv(x_card_sound_adc_handle_t *handle, uint32_t *mv)
{
    if (!handle || !mv) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t ret = x_card_sound_adc_read_raw(handle, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    if (handle->cali_enabled && handle->cali_handle) {
        int calibrated_mv = 0;
        ret = adc_cali_raw_to_voltage(handle->cali_handle, raw, &calibrated_mv);
        if (ret == ESP_OK) {
            *mv = (uint32_t)calibrated_mv;
            return ESP_OK;
        }
    }

    /* Fallback nominal conversion 0-4095 to 0-3300mV */
    *mv = ((uint32_t)raw * 3300U) / 4095U;
    return ESP_OK;
}

ESP_BOARD_ENTRY_IMPLEMENT(sound_adc, x_card_sound_adc_init, x_card_sound_adc_deinit);
