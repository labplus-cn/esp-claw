#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x_card_sound_adc_handle x_card_sound_adc_handle_t;

/**
 * @brief Init sound ADC device. Usually called by esp_board_manager.
 */
int x_card_sound_adc_init(void *cfg, int cfg_size, void **device_handle);

/**
 * @brief Deinit sound ADC device.
 */
int x_card_sound_adc_deinit(void *device_handle);

/**
 * @brief Read raw sound ADC value (0-4095).
 */
esp_err_t x_card_sound_adc_read_raw(x_card_sound_adc_handle_t *handle, int *raw);

/**
 * @brief Read sound ADC value in mV.
 */
esp_err_t x_card_sound_adc_read_mv(x_card_sound_adc_handle_t *handle, uint32_t *mv);

#ifdef __cplusplus
}
#endif
