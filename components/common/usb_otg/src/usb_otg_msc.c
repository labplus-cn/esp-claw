/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB OTG MSC Device mode — expose SD card as a USB mass storage device.
 *
 * The SD card FATFS is unmounted before entering MSC mode so the host PC
 * has exclusive block-level access.  FATFS is re-mounted when MSC stops.
 */

#include "usb_otg.h"

#include <stdlib.h>
#include <string.h>

#include "esp_board_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "tusb.h"

#include "dev_fs_fat.h"

static const char *TAG = "usb_otg_msc";

#define SDCARD_DEVICE_NAME   "fs_sdcard"
#define TINYUSB_RHPORT       1
#define TINYUSB_TASK_STOP_WAIT_MS 2000
#define SDCARD_RETRY_COUNT   3
#define SDCARD_RETRY_DELAY_MS 100

/* ---- SD card state for MSC callbacks ---- */

static sdmmc_card_t *s_card;
static sdmmc_host_t  s_host;
static char          s_mount_point[32];
static bool          s_msc_ejected;
static volatile bool s_msc_running;
static bool          s_raw_slot_initialized;
static bool          s_tinyusb_init_ok;
static dev_fs_fat_handle_t *s_fat_handle;
static sdmmc_slot_config_t s_slot_config;
static esp_vfs_fat_mount_config_t s_mount_config;
static TaskHandle_t  s_tinyusb_task_handle;
static SemaphoreHandle_t s_tinyusb_ready_sem;

/* ---- TinyUSB task ---- */

static void tinyusb_device_task(void *arg)
{
    (void)arg;

    const tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_HIGH,
    };
    s_tinyusb_init_ok = tusb_init(TINYUSB_RHPORT, &dev_init);
    xSemaphoreGive(s_tinyusb_ready_sem);

    if (!s_tinyusb_init_ok) {
        s_tinyusb_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_msc_running) {
        tud_task_ext(20, false);
    }

    (void)tusb_deinit(TINYUSB_RHPORT);
    s_tinyusb_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t msc_wait_for_tinyusb_task(void)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(TINYUSB_TASK_STOP_WAIT_MS);

    while (s_tinyusb_task_handle != NULL) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

/* ---- SD card unmount / remount helpers ---- */

static void msc_prepare_sdcard_config(const dev_fs_fat_config_t *config)
{
    const dev_fs_fat_sdmmc_sub_config_t *sdmmc_cfg = &config->sub_cfg.sdmmc;

    s_slot_config = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    s_slot_config.clk = sdmmc_cfg->pins.clk;
    s_slot_config.cmd = sdmmc_cfg->pins.cmd;
    s_slot_config.d0 = sdmmc_cfg->pins.d0;
    s_slot_config.d1 = sdmmc_cfg->pins.d1;
    s_slot_config.d2 = sdmmc_cfg->pins.d2;
    s_slot_config.d3 = sdmmc_cfg->pins.d3;
    s_slot_config.d4 = sdmmc_cfg->pins.d4;
    s_slot_config.d5 = sdmmc_cfg->pins.d5;
    s_slot_config.d6 = sdmmc_cfg->pins.d6;
    s_slot_config.d7 = sdmmc_cfg->pins.d7;
    s_slot_config.cd = sdmmc_cfg->pins.cd;
    s_slot_config.wp = sdmmc_cfg->pins.wp;
    s_slot_config.width = sdmmc_cfg->bus_width;
    s_slot_config.flags = sdmmc_cfg->slot_flags;

    s_mount_config = (esp_vfs_fat_mount_config_t) {
        .format_if_mount_failed = config->vfs_config.format_if_mount_failed,
        .max_files = config->vfs_config.max_files,
        .allocation_unit_size = config->vfs_config.allocation_unit_size,
    };
}

static esp_err_t msc_deinit_raw_sdcard(void)
{
    esp_err_t ret = ESP_OK;
    if (s_raw_slot_initialized) {
        ret = (s_host.flags & SDMMC_HOST_FLAG_DEINIT_ARG)
              ? s_host.deinit_p(s_host.slot)
              : s_host.deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to deinitialize raw SD card slot: %s", esp_err_to_name(ret));
            return ret;
        }
        s_raw_slot_initialized = false;
    }

    free(s_card);
    s_card = NULL;
    return ESP_OK;
}

static esp_err_t msc_mount_fatfs(void)
{
    ESP_RETURN_ON_FALSE(s_fat_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "SD card board handle is unavailable");

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= SDCARD_RETRY_COUNT; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(SDCARD_RETRY_DELAY_MS));

        sdmmc_card_t *mounted_card = NULL;
        ret = esp_vfs_fat_sdmmc_mount(s_mount_point, &s_host, &s_slot_config,
                                      &s_mount_config, &mounted_card);
        if (ret == ESP_OK) {
            s_fat_handle->card = mounted_card;
            ESP_LOGI(TAG, "FATFS re-mounted at %s", s_mount_point);
            s_fat_handle = NULL;
            return ESP_OK;
        }

        ESP_LOGW(TAG, "FATFS re-mount attempt %d/%d failed: %s",
                 attempt, SDCARD_RETRY_COUNT, esp_err_to_name(ret));
    }

    ESP_LOGE(TAG, "Failed to re-mount FATFS at %s: %s",
             s_mount_point, esp_err_to_name(ret));
    return ret;
}

static esp_err_t msc_acquire_sdcard(void)
{
    dev_fs_fat_handle_t *fat_hdl = NULL;
    esp_err_t err = esp_board_device_get_handle(SDCARD_DEVICE_NAME, (void **)&fat_hdl);
    ESP_RETURN_ON_ERROR(err, TAG, "SD card device '%s' not found", SDCARD_DEVICE_NAME);
    ESP_RETURN_ON_FALSE(fat_hdl && fat_hdl->card && fat_hdl->mount_point,
                        ESP_ERR_NOT_FOUND, TAG, "SD card not mounted");

    dev_fs_fat_config_t *config = NULL;
    err = esp_board_device_get_config(SDCARD_DEVICE_NAME, (void **)&config);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to get SD card configuration");
    ESP_RETURN_ON_FALSE(config && config->sub_type && strcmp(config->sub_type, "sdmmc") == 0,
                        ESP_ERR_NOT_SUPPORTED, TAG, "Only SDMMC-backed FATFS is supported");

    /* Save the exact Board Manager configuration before VFS frees its card object. */
    s_fat_handle = fat_hdl;
    s_host = fat_hdl->host;
    strlcpy(s_mount_point, fat_hdl->mount_point, sizeof(s_mount_point));
    msc_prepare_sdcard_config(config);

    err = esp_vfs_fat_sdcard_unmount(s_mount_point, fat_hdl->card);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to unmount FATFS at %s", s_mount_point);
    fat_hdl->card = NULL;

    s_card = calloc(1, sizeof(*s_card));
    if (!s_card) {
        err = ESP_ERR_NO_MEM;
        goto remount;
    }

    err = s_host.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SDMMC host: %s", esp_err_to_name(err));
        goto remount;
    }

    err = sdmmc_host_init_slot(s_host.slot, &s_slot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SDMMC slot %d: %s",
                 s_host.slot, esp_err_to_name(err));
        goto remount;
    }
    s_raw_slot_initialized = true;

    err = sdmmc_card_init(&s_host, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize raw SD card: %s", esp_err_to_name(err));
        goto remount;
    }

    ESP_LOGI(TAG, "FATFS unmounted at %s, SD card ready for MSC", s_mount_point);
    return ESP_OK;

remount:
    (void)msc_deinit_raw_sdcard();
    (void)msc_mount_fatfs();
    return err;
}

static esp_err_t msc_release_sdcard(void)
{
    if (!s_card && !s_fat_handle) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(msc_deinit_raw_sdcard(), TAG, "Failed to release raw SD card");
    return msc_mount_fatfs();
}

/* ---- TinyUSB MSC callbacks ---- */

/* SCSI Inquiry: vendor / product / revision */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "ESP-Claw",  8);
    memcpy(product_id,  "SD Card         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

/* Test Unit Ready */
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (s_msc_ejected || s_card == NULL) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
        return false;
    }
    return true;
}

/* Read Capacity */
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    if (s_card) {
        *block_count = s_card->csd.capacity;
        *block_size  = s_card->csd.sector_size;
    } else {
        *block_count = 0;
        *block_size  = 512;
    }
}

/* Start/Stop unit (eject/load) */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    if (load_eject) {
        s_msc_ejected = !start;
    }
    return true;
}

/* READ10 — copy sectors from SD card to USB buffer */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;

    if (!s_card) {
        return TUD_MSC_RET_ERROR;
    }

    const uint32_t sector_size = s_card->csd.sector_size;
    if (offset != 0 || bufsize == 0 || (bufsize % sector_size) != 0) {
        return TUD_MSC_RET_ERROR;
    }

    uint32_t sector_count = bufsize / sector_size;
    if (lba >= (uint32_t)s_card->csd.capacity ||
            sector_count > (uint32_t)s_card->csd.capacity - lba) {
        return TUD_MSC_RET_ERROR;
    }

    esp_err_t err = sdmmc_read_sectors(s_card, buffer, lba, sector_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_read_sectors(lba=%lu, count=%lu) failed: %s",
                 (unsigned long)lba, (unsigned long)sector_count, esp_err_to_name(err));
        return TUD_MSC_RET_ERROR;
    }
    return (int32_t)bufsize;
}

/* WRITE10 — copy sectors from USB buffer to SD card */
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;

    if (!s_card) {
        return TUD_MSC_RET_ERROR;
    }

    const uint32_t sector_size = s_card->csd.sector_size;
    if (offset != 0 || bufsize == 0 || (bufsize % sector_size) != 0) {
        return TUD_MSC_RET_ERROR;
    }

    uint32_t sector_count = bufsize / sector_size;
    if (lba >= (uint32_t)s_card->csd.capacity ||
            sector_count > (uint32_t)s_card->csd.capacity - lba) {
        return TUD_MSC_RET_ERROR;
    }

    esp_err_t err = sdmmc_write_sectors(s_card, buffer, lba, sector_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_write_sectors(lba=%lu, count=%lu) failed: %s",
                 (unsigned long)lba, (unsigned long)sector_count, esp_err_to_name(err));
        return TUD_MSC_RET_ERROR;
    }
    return (int32_t)bufsize;
}

/* SCSI commands not covered by the standard callbacks */
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

/* ---- Role start / stop (called by usb_otg_core.c) ---- */

esp_err_t usb_otg_msc_start(usb_phy_handle_t phy)
{
    (void)phy;

    if (s_msc_running) {
        return ESP_OK;
    }

    s_msc_ejected = false;

    /* Unmount FATFS and grab raw SD card handles */
    esp_err_t ret = msc_acquire_sdcard();
    if (ret != ESP_OK) {
        return ret;
    }

    s_tinyusb_ready_sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(s_tinyusb_ready_sem != NULL, ESP_ERR_NO_MEM, remount,
                      TAG, "Failed to create TinyUSB ready semaphore");

    s_tinyusb_init_ok = false;
    s_msc_running = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(tinyusb_device_task, "tinyusb_msc",
                                                   CONFIG_USB_OTG_MSC_TASK_STACK_SIZE,
                                                   NULL, 5, &s_tinyusb_task_handle, 0);
    if (task_ret != pdTRUE) {
        s_msc_running = false;
        ret = ESP_ERR_NO_MEM;
        goto delete_sem;
    }

    /* tusb_init() is synchronous. Waiting indefinitely here keeps the ready
     * semaphore alive until the TinyUSB task has finished using it. */
    xSemaphoreTake(s_tinyusb_ready_sem, portMAX_DELAY);
    vSemaphoreDelete(s_tinyusb_ready_sem);
    s_tinyusb_ready_sem = NULL;

    if (!s_tinyusb_init_ok) {
        ESP_LOGE(TAG, "tusb_init failed on rhport %d", TINYUSB_RHPORT);
        s_msc_running = false;
        ret = ESP_FAIL;
        goto wait_task;
    }

    ESP_LOGI(TAG, "MSC device mode started (SD card -> USB U-disk)");
    return ESP_OK;

wait_task:
    if (msc_wait_for_tinyusb_task() != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB task did not exit; keeping SD card in raw mode");
        return ESP_ERR_TIMEOUT;
    }
delete_sem:
    if (s_tinyusb_ready_sem) {
        vSemaphoreDelete(s_tinyusb_ready_sem);
        s_tinyusb_ready_sem = NULL;
    }
remount:
    (void)msc_release_sdcard();
    return ret;
}

esp_err_t usb_otg_msc_stop(void)
{
    if (!s_msc_running && s_tinyusb_task_handle == NULL && s_fat_handle == NULL) {
        return ESP_OK;
    }

    s_msc_running = false;

    /* MSC callbacks can access s_card, so do not release it until the TinyUSB
     * task has deinitialized the device stack and stopped dispatching events. */
    esp_err_t ret = msc_wait_for_tinyusb_task();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB task did not exit; SD card remains in raw mode");
        return ret;
    }

    /* Re-mount FATFS so the firmware can use the SD card again */
    ret = msc_release_sdcard();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card re-mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MSC device mode stopped");
    return ret;
}
