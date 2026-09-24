/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB OTG operating role.
 */
typedef enum {
    USB_OTG_ROLE_NONE,    /**< No USB stack active */
    USB_OTG_ROLE_HOST,    /**< USB Host mode (e.g. UVC camera) */
    USB_OTG_ROLE_DEVICE,  /**< USB Device mode (e.g. MSC U-disk) */
} usb_otg_role_t;

/**
 * @brief Initialize the USB OTG subsystem (PHY, lock, state).
 *
 * Safe to call multiple times; subsequent calls are no-ops.
 *
 * @return ESP_OK on success
 */
esp_err_t usb_otg_init(void);

/**
 * @brief Tear down the USB OTG subsystem completely.
 *
 * Stops any active role, releases the PHY and frees resources.
 *
 * @return ESP_OK on success
 */
esp_err_t usb_otg_deinit(void);

/**
 * @brief Switch the USB OTG port to a new role.
 *
 * If the requested role is already active, this is a no-op.
 * Switching from one active role to another stops the current
 * role first, then starts the new one.
 *
 * @param[in] new_role  Target role (HOST, DEVICE, or NONE).
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE on failure.
 */
esp_err_t usb_otg_switch_role(usb_otg_role_t new_role);

/**
 * @brief Return the currently active USB OTG role.
 */
usb_otg_role_t usb_otg_get_role(void);

#ifdef __cplusplus
}
#endif
