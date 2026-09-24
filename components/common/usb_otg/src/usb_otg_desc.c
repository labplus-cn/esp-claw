/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Device descriptors for TinyUSB MSC mode.
 *
 * The espressif/tinyusb component auto-generates tusb_config.h from Kconfig.
 * This file provides the device / configuration / string descriptors that
 * TinyUSB needs when operating as an MSC device (SD card U-disk).
 */

#include "sdkconfig.h"

#if CONFIG_USB_OTG_MSC

#include "tusb.h"

/* ---- Device Descriptor ---- */

static const tusb_desc_device_t s_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,   /* USB 2.0 High-Speed */
    .bDeviceClass       = 0x00,     /* Defined at interface level */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x303A,   /* Espressif */
    .idProduct          = 0x4001,   /* Custom MSC */
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

/* ---- Configuration Descriptor ---- */

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
#define EPNUM_MSC_OUT     0x01
#define EPNUM_MSC_IN      0x81

static const uint8_t s_fs_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static const uint8_t s_hs_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 512),
};

static const tusb_desc_device_qualifier_t s_device_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .bReserved          = 0,
};

static uint8_t s_other_speed_configuration[CONFIG_TOTAL_LEN];

/* ---- String Descriptors ---- */

static const char *s_string_descriptor[] = {
    [0] = (const char[]){0x09, 0x04},   /* English (US) */
    [1] = "ESP-Claw",                     /* Manufacturer */
    [2] = "ESP-Claw SD Card Reader",      /* Product */
    [3] = "000001",                       /* Serial */
};

/* ---- TinyUSB descriptor callback ---- */

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_device_descriptor;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return (tud_speed_get() == TUSB_SPEED_HIGH) ? s_hs_configuration_descriptor
                                                 : s_fs_configuration_descriptor;
}

const uint8_t *tud_descriptor_device_qualifier_cb(void)
{
    return (const uint8_t *)&s_device_qualifier;
}

const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;

    const uint8_t *descriptor = (tud_speed_get() == TUSB_SPEED_HIGH)
                                ? s_fs_configuration_descriptor
                                : s_hs_configuration_descriptor;
    memcpy(s_other_speed_configuration, descriptor, CONFIG_TOTAL_LEN);
    s_other_speed_configuration[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return s_other_speed_configuration;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    static uint16_t utf16_buf[32 + 1];  /* +1 for length prefix word */

    if (index == 0) {
        utf16_buf[0] = (TUSB_DESC_STRING << 8) | 4;
        memcpy(&utf16_buf[1], s_string_descriptor[0], 2);
        return utf16_buf;
    }

    if (index >= sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0])) {
        return NULL;
    }

    const char *str = s_string_descriptor[index];
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    if (len > 32) {
        len = 32;
    }

    utf16_buf[0] = (TUSB_DESC_STRING << 8) | (uint16_t)(2 * len + 2);
    for (size_t i = 0; i < len; i++) {
        utf16_buf[1 + i] = (uint16_t)str[i];
    }
    return utf16_buf;
}

#endif /* CONFIG_USB_OTG_MSC */
