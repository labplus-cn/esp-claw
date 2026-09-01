/*
 * SPDX-FileCopyrightText: 2026 X-Card
 * SPDX-License-Identifier: MIT
 *
 * X-Card power monitor custom devices.
 * X-Card 电源监测 custom device 驱动。
 *
 * GPIO5/ADC1_CH4 uses the original ADC_ATTEN_DB_12 configuration
 * (approximately 0-3.3 V theoretical range). With Q5 enabled, the
 * 100K/100K board divider feeds half of VBAT to the ADC: 0-2.1 V at
 * ADC1_CH4 for a 0-4.2 V VBAT range.
 * GPIO5/ADC1_CH4 保持原来的 ADC_ATTEN_DB_12 配置（理论范围约为 0-3.3 V）。
 * Q5 导通时，板载 100K/100K 分压将 VBAT 的一半输入 ADC：
 * 0-4.2 V 电池电压对应 ADC1_CH4 的 0-2.1 V。
 */

#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_board_manager_includes.h"
#include "esp_log.h"
#include "periph_adc.h"
#include "periph_gpio.h"
#include "x_card_power.h"

static const char *TAG = "x_power";

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t       adc_unit;
    int8_t       channel;
    int8_t       bitwidth;
    int8_t       attenuation;
    int8_t       samples;
    int8_t       sampling_freq_hz;
    const char *description;
    uint8_t      peripheral_count;
    const char *peripheral_name;
} x_card_battery_adc_config_t;

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t       gpio;
    int8_t       active_level;
    int8_t       debounce_ms;
    const char *description;
    uint8_t      peripheral_count;
    const char *peripheral_name;
} x_card_gpio_detector_config_t;

#define X_CARD_BATTERY_ADC_PERIPH_NAME  "adc_battery"
#define X_CARD_BATTERY_ADC_CHANNEL      ADC_CHANNEL_4
#define X_CARD_BATTERY_ADC_MAX_RAW      4095U
#define X_CARD_BATTERY_ADC_FULL_SCALE_MV 3300U
#define X_CARD_BATTERY_ADC_SAMPLES      8U
#define X_CARD_BATTERY_ADC_VBAT_NUM     2U   /* R39 + R40 = 200K */
#define X_CARD_BATTERY_ADC_VBAT_DEN     1U   /* R40 = 100K */
#define X_CARD_VBUS_PERIPH_NAME         "gpio_vbus"
#define X_CARD_VBUS_ACTIVE_LEVEL        1

struct x_card_battery_adc_handle {
    periph_adc_handle_t *adc_periph;
    adc_channel_t        channel;
    adc_cali_handle_t    cali_handle;
    uint8_t              sample_count;
    adc_atten_t          atten;
    bool                 cali_enabled;
};

struct x_card_vbus_detector_handle {
    periph_gpio_handle_t *gpio_periph;
    int                   active_level;
};

#define X_CARD_BASE_POWER_PERIPH_NAME   "gpio_base_power"
#define X_CARD_BASE_DETECT_PERIPH_NAME  "gpio_base_detect"
#define X_CARD_CHARGING_STATUS_PERIPH_NAME "gpio_charging_status"

struct x_card_base_power_ctrl_handle {
    periph_gpio_handle_t *gpio_periph;
    int                   active_level;
    bool                  power_state;
};

struct x_card_base_detector_handle {
    periph_gpio_handle_t *gpio_periph;
    int                   active_level;
};

struct x_card_charging_detector_handle {
    periph_gpio_handle_t *gpio_periph;
    int                   active_level;
};

int x_card_battery_adc_init(void *cfg, int cfg_size, void **device_handle)
{
    if (!device_handle) {
        return -1;
    }

    periph_adc_handle_t *adc_periph = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle(X_CARD_BATTERY_ADC_PERIPH_NAME,
                                                        (void **)&adc_periph);
    if (ret != ESP_OK || !adc_periph || !adc_periph->oneshot) {
        ESP_LOGE(TAG, "get %s peripheral failed: %s",
                 X_CARD_BATTERY_ADC_PERIPH_NAME, esp_err_to_name(ret));
        return -1;
    }

    x_card_battery_adc_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return -1;
    }

    handle->adc_periph = adc_periph;
    handle->channel = X_CARD_BATTERY_ADC_CHANNEL;
    handle->sample_count = X_CARD_BATTERY_ADC_SAMPLES;
    handle->atten = ADC_ATTEN_DB_12;

    if (cfg && cfg_size >= (int)sizeof(x_card_battery_adc_config_t)) {
        const x_card_battery_adc_config_t *battery_cfg = cfg;
        if (battery_cfg->samples > 0) {
            handle->sample_count = (uint8_t)battery_cfg->samples;
        }
        if (battery_cfg->attenuation >= ADC_ATTEN_DB_0 &&
            battery_cfg->attenuation <= ADC_ATTEN_DB_12) {
            handle->atten = (adc_atten_t)battery_cfg->attenuation;
        }
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = handle->channel,
        .atten = handle->atten,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &handle->cali_handle);
    if (ret == ESP_OK) {
        handle->cali_enabled = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable, using nominal conversion: %s",
                 esp_err_to_name(ret));
    }

    *device_handle = handle;

    ESP_LOGI(TAG, "battery_adc ready on %s channel %d, atten=%d, samples=%u, calibrated=%s",
             X_CARD_BATTERY_ADC_PERIPH_NAME, handle->channel,
             handle->atten, handle->sample_count,
             handle->cali_enabled ? "yes" : "no");
    return 0;
}

int x_card_battery_adc_deinit(void *device_handle)
{
    x_card_battery_adc_handle_t *handle = device_handle;
    if (handle && handle->cali_enabled && handle->cali_handle) {
        adc_cali_delete_scheme_curve_fitting(handle->cali_handle);
    }
    free(handle);
    ESP_LOGI(TAG, "battery_adc deinit");
    return 0;
}

esp_err_t x_card_battery_adc_read_raw(x_card_battery_adc_handle_t *handle, int *raw)
{
    if (!handle || !raw || !handle->adc_periph || !handle->adc_periph->oneshot) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t sum = 0;
    uint8_t sample_count = handle->sample_count > 0 ? handle->sample_count : 1;
    for (uint8_t i = 0; i < sample_count; ++i) {
        int sample = 0;
        esp_err_t ret = adc_oneshot_read(handle->adc_periph->oneshot,
                                         handle->channel, &sample);
        if (ret != ESP_OK) {
            return ret;
        }
        sum += (uint32_t)sample;
    }

    *raw = (int)(sum / sample_count);
    return ESP_OK;
}

static esp_err_t battery_raw_to_pin_mv(x_card_battery_adc_handle_t *handle,
                                       int raw, uint32_t *mv)
{
    if (!handle || !mv) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->cali_enabled && handle->cali_handle) {
        int calibrated_mv = 0;
        esp_err_t ret = adc_cali_raw_to_voltage(handle->cali_handle, raw, &calibrated_mv);
        if (ret == ESP_OK) {
            *mv = (uint32_t)calibrated_mv;
            return ESP_OK;
        }
        ESP_LOGD(TAG, "Calibrated conversion failed, using nominal conversion: %s",
                 esp_err_to_name(ret));
    }

    *mv = ((uint32_t)raw * X_CARD_BATTERY_ADC_FULL_SCALE_MV) /
          X_CARD_BATTERY_ADC_MAX_RAW;
    return ESP_OK;
}

esp_err_t x_card_battery_adc_read(x_card_battery_adc_handle_t *handle,
                                  int *raw, uint32_t *pin_mv, uint32_t *battery_mv)
{
    if (!handle || (!raw && !pin_mv && !battery_mv)) {
        return ESP_ERR_INVALID_ARG;
    }

    int averaged_raw = 0;
    esp_err_t ret = x_card_battery_adc_read_raw(handle, &averaged_raw);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t converted_pin_mv = 0;
    ret = battery_raw_to_pin_mv(handle, averaged_raw, &converted_pin_mv);
    if (ret != ESP_OK) {
        return ret;
    }

    if (raw) {
        *raw = averaged_raw;
    }
    if (pin_mv) {
        *pin_mv = converted_pin_mv;
    }
    if (battery_mv) {
        *battery_mv = (converted_pin_mv * X_CARD_BATTERY_ADC_VBAT_NUM) /
                      X_CARD_BATTERY_ADC_VBAT_DEN;
    }
    return ESP_OK;
}

esp_err_t x_card_battery_adc_read_pin_mv(x_card_battery_adc_handle_t *handle, uint32_t *mv)
{
    return x_card_battery_adc_read(handle, NULL, mv, NULL);
}

esp_err_t x_card_battery_adc_read_battery_mv(x_card_battery_adc_handle_t *handle, uint32_t *mv)
{
    return x_card_battery_adc_read(handle, NULL, NULL, mv);
}

int x_card_vbus_detector_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg;
    (void)cfg_size;

    if (!device_handle) {
        return -1;
    }

    periph_gpio_handle_t *gpio_periph = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle(X_CARD_VBUS_PERIPH_NAME,
                                                        (void **)&gpio_periph);
    if (ret != ESP_OK || !gpio_periph) {
        ESP_LOGE(TAG, "get %s peripheral failed: %s",
                 X_CARD_VBUS_PERIPH_NAME, esp_err_to_name(ret));
        return -1;
    }

    x_card_vbus_detector_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return -1;
    }

    handle->gpio_periph = gpio_periph;
    handle->active_level = X_CARD_VBUS_ACTIVE_LEVEL;
    *device_handle = handle;

    ESP_LOGI(TAG, "vbus_detector ready on GPIO%d active_level=%d",
             gpio_periph->gpio_num, handle->active_level);
    return 0;
}

int x_card_vbus_detector_deinit(void *device_handle)
{
    free(device_handle);
    ESP_LOGI(TAG, "vbus_detector deinit");
    return 0;
}

esp_err_t x_card_vbus_detector_is_present(x_card_vbus_detector_handle_t *handle, bool *present)
{
    if (!handle || !present || !handle->gpio_periph) {
        return ESP_ERR_INVALID_ARG;
    }

    int level = gpio_get_level(handle->gpio_periph->gpio_num);
    *present = (level == handle->active_level);
    return ESP_OK;
}

int x_card_base_power_ctrl_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg;
    (void)cfg_size;

    if (!device_handle) {
        return -1;
    }

    periph_gpio_handle_t *gpio_periph = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle(X_CARD_BASE_POWER_PERIPH_NAME,
                                                        (void **)&gpio_periph);
    if (ret != ESP_OK || !gpio_periph) {
        ESP_LOGE(TAG, "get %s peripheral failed: %s",
                 X_CARD_BASE_POWER_PERIPH_NAME, esp_err_to_name(ret));
        return -1;
    }

    x_card_base_power_ctrl_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return -1;
    }

    handle->gpio_periph = gpio_periph;
    handle->active_level = 1; // High active for power enable
    handle->power_state = false;
    *device_handle = handle;

    ESP_LOGI(TAG, "base_power_ctrl ready on GPIO%d active_level=%d",
             gpio_periph->gpio_num, handle->active_level);
    return 0;
}

int x_card_base_power_ctrl_deinit(void *device_handle)
{
    free(device_handle);
    ESP_LOGI(TAG, "base_power_ctrl deinit");
    return 0;
}

esp_err_t x_card_base_power_ctrl_set(x_card_base_power_ctrl_handle_t *handle, bool enable)
{
    if (!handle || !handle->gpio_periph) {
        return ESP_ERR_INVALID_ARG;
    }

    int level = enable ? handle->active_level : !handle->active_level;
    esp_err_t ret = gpio_set_level(handle->gpio_periph->gpio_num, level);
    if (ret == ESP_OK) {
        handle->power_state = enable;
    }
    return ret;
}

esp_err_t x_card_base_power_ctrl_get(x_card_base_power_ctrl_handle_t *handle, bool *enabled)
{
    if (!handle || !enabled) {
        return ESP_ERR_INVALID_ARG;
    }

    *enabled = handle->power_state;
    return ESP_OK;
}

int x_card_base_detector_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg;
    (void)cfg_size;

    if (!device_handle) {
        return -1;
    }

    periph_gpio_handle_t *gpio_periph = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle(X_CARD_BASE_DETECT_PERIPH_NAME,
                                                        (void **)&gpio_periph);
    if (ret != ESP_OK || !gpio_periph) {
        ESP_LOGE(TAG, "get %s peripheral failed: %s",
                 X_CARD_BASE_DETECT_PERIPH_NAME, esp_err_to_name(ret));
        return -1;
    }

    x_card_base_detector_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return -1;
    }

    handle->gpio_periph = gpio_periph;
    handle->active_level = 0; // Low active = base inserted
    *device_handle = handle;

    ESP_LOGI(TAG, "base_detector ready on GPIO%d active_level=%d",
             gpio_periph->gpio_num, handle->active_level);
    return 0;
}

int x_card_base_detector_deinit(void *device_handle)
{
    free(device_handle);
    ESP_LOGI(TAG, "base_detector deinit");
    return 0;
}

esp_err_t x_card_base_detector_is_present(x_card_base_detector_handle_t *handle, bool *present)
{
    if (!handle || !present || !handle->gpio_periph) {
        return ESP_ERR_INVALID_ARG;
    }

    int level = gpio_get_level(handle->gpio_periph->gpio_num);
    *present = (level == handle->active_level);
    return ESP_OK;
}

int x_card_charging_detector_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg;
    (void)cfg_size;

    if (!device_handle) {
        return -1;
    }

    periph_gpio_handle_t *gpio_periph = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle(X_CARD_CHARGING_STATUS_PERIPH_NAME,
                                                        (void **)&gpio_periph);
    if (ret != ESP_OK || !gpio_periph) {
        ESP_LOGE(TAG, "get %s peripheral failed: %s",
                 X_CARD_CHARGING_STATUS_PERIPH_NAME, esp_err_to_name(ret));
        return -1;
    }

    x_card_charging_detector_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return -1;
    }

    handle->gpio_periph = gpio_periph;
    handle->active_level = 0; // Low active = charging completed
    *device_handle = handle;

    ESP_LOGI(TAG, "charging_detector ready on GPIO%d active_level=%d",
             gpio_periph->gpio_num, handle->active_level);
    return 0;
}

int x_card_charging_detector_deinit(void *device_handle)
{
    free(device_handle);
    ESP_LOGI(TAG, "charging_detector deinit");
    return 0;
}

esp_err_t x_card_charging_detector_is_completed(x_card_charging_detector_handle_t *handle, bool *completed)
{
    if (!handle || !completed || !handle->gpio_periph) {
        return ESP_ERR_INVALID_ARG;
    }

    int level = gpio_get_level(handle->gpio_periph->gpio_num);
    *completed = (level == handle->active_level);
    return ESP_OK;
}

ESP_BOARD_ENTRY_IMPLEMENT(battery_adc, x_card_battery_adc_init, x_card_battery_adc_deinit);
ESP_BOARD_ENTRY_IMPLEMENT(vbus_detector, x_card_vbus_detector_init, x_card_vbus_detector_deinit);
ESP_BOARD_ENTRY_IMPLEMENT(base_power_ctrl, x_card_base_power_ctrl_init, x_card_base_power_ctrl_deinit);
ESP_BOARD_ENTRY_IMPLEMENT(base_detector, x_card_base_detector_init, x_card_base_detector_deinit);
ESP_BOARD_ENTRY_IMPLEMENT(charging_detector, x_card_charging_detector_init, x_card_charging_detector_deinit);
