/*
 * SPDX-FileCopyrightText: 2026 X-Card
 * SPDX-License-Identifier: MIT
 *
 * X-Card power monitor custom devices.
 * X-Card 电源监测 custom device 驱动。
 *
 * With Q5 enabled, GPIO5/ADC1_CH4 samples half of VBAT through a
 * 100K/100K divider. The restored battery voltage is ADC pin voltage * 2.
 * Q5 导通时，GPIO5/ADC1_CH4 通过 100K/100K 分压采样 VBAT 的一半。
 * 还原电池电压 = ADC 引脚电压 * 2。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_board_manager_includes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x_card_battery_adc_handle x_card_battery_adc_handle_t;
typedef struct x_card_vbus_detector_handle x_card_vbus_detector_handle_t;
typedef struct x_card_base_power_ctrl_handle x_card_base_power_ctrl_handle_t;
typedef struct x_card_base_detector_handle x_card_base_detector_handle_t;
typedef struct x_card_charging_detector_handle x_card_charging_detector_handle_t;

/**
 * @brief Read raw battery ADC value.
 * @brief 读取电池 ADC 原始值。
 */
esp_err_t x_card_battery_adc_read_raw(x_card_battery_adc_handle_t *handle, int *raw);

/**
 * @brief Read approximate ADC pin voltage in millivolts.
 * @brief 读取 ADC 引脚近似电压，单位 mV。
 *
 * With Q5 enabled, this is half of VBAT at GPIO5/ADC1_CH4.
 * Q5 导通时，这里返回 GPIO5/ADC1_CH4 引脚电压，即 VBAT 的一半。
 */
esp_err_t x_card_battery_adc_read_pin_mv(x_card_battery_adc_handle_t *handle, uint32_t *mv);

/**
 * @brief Read restored battery voltage in millivolts.
 * @brief 读取还原后的电池电压，单位 mV。
 */
esp_err_t x_card_battery_adc_read_battery_mv(x_card_battery_adc_handle_t *handle, uint32_t *mv);

/**
 * @brief Read one averaged battery sample and its converted voltages.
 * @brief 读取一组平均后的电池采样值及其换算电压。
 */
esp_err_t x_card_battery_adc_read(x_card_battery_adc_handle_t *handle,
                                  int *raw, uint32_t *pin_mv, uint32_t *battery_mv);

/**
 * @brief Check whether Type-C VBUS is present.
 * @brief 检测 Type-C VBUS 是否存在。
 */
esp_err_t x_card_vbus_detector_is_present(x_card_vbus_detector_handle_t *handle, bool *present);

/**
 * @brief Set base power status.
 * @brief 设置底座电源供电状态。
 */
esp_err_t x_card_base_power_ctrl_set(x_card_base_power_ctrl_handle_t *handle, bool enable);

/**
 * @brief Get base power status.
 * @brief 获取底座电源供电状态。
 */
esp_err_t x_card_base_power_ctrl_get(x_card_base_power_ctrl_handle_t *handle, bool *enabled);

/**
 * @brief Check whether base dock is connected.
 * @brief 检测底座是否连接。
 */
esp_err_t x_card_base_detector_is_present(x_card_base_detector_handle_t *handle, bool *present);

/**
 * @brief Check whether charging status completed.
 * @brief 检测是否充满电（低电平表示充满）。
 */
esp_err_t x_card_charging_detector_is_completed(x_card_charging_detector_handle_t *handle, bool *completed);

int x_card_battery_adc_init(void *cfg, int cfg_size, void **device_handle);
int x_card_battery_adc_deinit(void *device_handle);
int x_card_vbus_detector_init(void *cfg, int cfg_size, void **device_handle);
int x_card_vbus_detector_deinit(void *device_handle);
int x_card_base_power_ctrl_init(void *cfg, int cfg_size, void **device_handle);
int x_card_base_power_ctrl_deinit(void *device_handle);
int x_card_base_detector_init(void *cfg, int cfg_size, void **device_handle);
int x_card_base_detector_deinit(void *device_handle);
int x_card_charging_detector_init(void *cfg, int cfg_size, void **device_handle);
int x_card_charging_detector_deinit(void *device_handle);

#ifdef __cplusplus
}
#endif
