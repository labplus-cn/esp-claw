/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB OTG core: PHY management and role-switching state machine.
 */

#include "usb_otg.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_private/usb_phy.h"

static const char *TAG = "usb_otg_core";

/* ---- Internal state ---- */

typedef struct {
    usb_phy_handle_t phy;
    usb_otg_role_t   role;
    SemaphoreHandle_t lock;
    bool              initialized;
} usb_otg_state_t;

static usb_otg_state_t s_otg;

/* ---- Forward declarations for role-specific start/stop ---- */

#if CONFIG_USB_OTG_MSC
esp_err_t usb_otg_msc_start(usb_phy_handle_t phy);
esp_err_t usb_otg_msc_stop(void);
#else
static inline esp_err_t usb_otg_msc_start(usb_phy_handle_t phy) { (void)phy; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t usb_otg_msc_stop(void) { return ESP_OK; }
#endif

#if CONFIG_USB_OTG_HOST_UVC
esp_err_t usb_otg_host_start(usb_phy_handle_t phy);
esp_err_t usb_otg_host_stop(void);
#else
static inline esp_err_t usb_otg_host_start(usb_phy_handle_t phy) { (void)phy; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t usb_otg_host_stop(void) { return ESP_OK; }
#endif

/* ---- PHY helpers ---- */

static esp_err_t usb_otg_phy_create(usb_phy_target_t target, usb_phy_controller_t ctrl,
                                     usb_otg_role_t role, usb_phy_handle_t *out_phy)
{
    usb_phy_config_t phy_cfg = {
        .controller = ctrl,
        .target = target,
        .otg_mode = (role == USB_OTG_ROLE_HOST) ? USB_OTG_MODE_HOST : USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,
    };

    esp_err_t ret = usb_new_phy(&phy_cfg, out_phy);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create USB PHY: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t usb_otg_phy_delete(usb_phy_handle_t phy)
{
    if (phy) {
        return usb_del_phy(phy);
    }
    return ESP_OK;
}

/* ---- Public API ---- */

esp_err_t usb_otg_init(void)
{
    if (s_otg.initialized) {
        return ESP_OK;
    }

    s_otg.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_otg.lock != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create OTG lock");

    s_otg.role = USB_OTG_ROLE_NONE;
    s_otg.phy = NULL;
    s_otg.initialized = true;

    ESP_LOGI(TAG, "USB OTG subsystem initialized");
    return ESP_OK;
}

esp_err_t usb_otg_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_otg.initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    /* Stop any active role first */
    esp_err_t ret = usb_otg_switch_role(USB_OTG_ROLE_NONE);
    if (ret != ESP_OK) {
        return ret;
    }

    vSemaphoreDelete(s_otg.lock);
    s_otg.lock = NULL;
    s_otg.initialized = false;

    ESP_LOGI(TAG, "USB OTG subsystem deinitialized");
    return ESP_OK;
}

esp_err_t usb_otg_switch_role(usb_otg_role_t new_role)
{
    ESP_RETURN_ON_FALSE(s_otg.initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    xSemaphoreTake(s_otg.lock, portMAX_DELAY);

    if (s_otg.role == new_role) {
        ESP_LOGD(TAG, "Already in role %d, nothing to do", new_role);
        xSemaphoreGive(s_otg.lock);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Switching role: %d -> %d", s_otg.role, new_role);

    /* Step 1: stop the current role */
    esp_err_t ret = ESP_OK;
    switch (s_otg.role) {
    case USB_OTG_ROLE_DEVICE:
        ret = usb_otg_msc_stop();
        break;
    case USB_OTG_ROLE_HOST:
        ret = usb_otg_host_stop();
        break;
    case USB_OTG_ROLE_NONE:
    default:
        break;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop current role %d: %s", s_otg.role, esp_err_to_name(ret));
        xSemaphoreGive(s_otg.lock);
        return ret;
    }

    /* Step 2: release old PHY if any */
    if (s_otg.phy) {
        usb_otg_phy_delete(s_otg.phy);
        s_otg.phy = NULL;
    }

    s_otg.role = USB_OTG_ROLE_NONE;

    /* Step 3: start the new role if not NONE */
    if (new_role != USB_OTG_ROLE_NONE) {
        usb_phy_target_t target = USB_PHY_TARGET_UTMI;   /* ESP32-P4 internal UTMI PHY */
        usb_phy_controller_t ctrl = USB_PHY_CTRL_OTG;

        ret = usb_otg_phy_create(target, ctrl, new_role, &s_otg.phy);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PHY create failed for role %d", new_role);
            xSemaphoreGive(s_otg.lock);
            return ret;
        }

        switch (new_role) {
        case USB_OTG_ROLE_DEVICE:
            ret = usb_otg_msc_start(s_otg.phy);
            break;
        case USB_OTG_ROLE_HOST:
            ret = usb_otg_host_start(s_otg.phy);
            break;
        default:
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start role %d: %s", new_role, esp_err_to_name(ret));
            usb_otg_phy_delete(s_otg.phy);
            s_otg.phy = NULL;
            xSemaphoreGive(s_otg.lock);
            return ret;
        }

        s_otg.role = new_role;
    }

    xSemaphoreGive(s_otg.lock);
    ESP_LOGI(TAG, "USB OTG role is now %d", s_otg.role);
    return ESP_OK;
}

usb_otg_role_t usb_otg_get_role(void)
{
    return s_otg.role;
}
