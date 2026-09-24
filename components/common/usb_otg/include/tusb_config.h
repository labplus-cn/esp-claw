/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * TinyUSB configuration for ESP-Claw USB OTG component.
 * This file is picked up from the usb_otg component's include directory.
 */

#pragma once

#include "sdkconfig.h"
#include "esp_attr.h"

/* ESP32-P4 maps TinyUSB rhport 1 to the DWC2 HS controller and UTMI PHY. */
#define CFG_TUSB_RHPORT1_MODE    (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)

/* ---- Common ---- */
#define CFG_TUSB_OS              OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH     freertos/

/* Match ESP-IDF's ESP32-P4 DWC2 HS setup: DMA buffers must stay in internal
 * DRAM and cache maintenance must use the configured L1 cache-line size. */
#define CFG_TUD_DWC2_SLAVE_ENABLE    0
#define CFG_TUD_DWC2_DMA_ENABLE      1
#define CFG_TUD_MEM_DCACHE_ENABLE    1
#define CFG_TUD_MEM_DCACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CFG_TUSB_MEM_SECTION         DRAM_DMA_ALIGNED_ATTR
#define CFG_TUSB_MEM_ALIGN           __attribute__((aligned(CONFIG_CACHE_L1_CACHE_LINE_SIZE)))

/* ---- Device stack ---- */
#define CFG_TUD_ENABLED          1
#define CFG_TUD_ENDPOINT0_SIZE   64

/* ---- MSC class (SD card U-disk) ---- */
#ifdef CONFIG_USB_OTG_MSC
#define CFG_TUD_MSC              1
#define CFG_TUD_MSC_EP_BUFSIZE   4096
#else
#define CFG_TUD_MSC              0
#endif

/* ---- All other device classes disabled ---- */
#define CFG_TUD_CDC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           0
#define CFG_TUD_DFU              0
#define CFG_TUD_ECM_RNDIS        0
#define CFG_TUD_NCM              0
#define CFG_TUD_UVC              0
#define CFG_TUD_AUDIO            0
#define CFG_TUD_VIDEO            0
#define CFG_TUD_BTH              0
#define CFG_TUD_HID_BOOT         0
