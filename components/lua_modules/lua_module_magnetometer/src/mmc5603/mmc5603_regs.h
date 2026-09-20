/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MMC5603NJ register definitions.
 * Based on official datasheet Rev.B
 */

#pragma once

/* I2C address (7-bit): 0x30, Write: 0x60, Read: 0x61 */
#define MMC5603_I2C_ADDR         0x30

/* Data registers (20-bit per axis, MSB first) */
#define MMC5603_REG_XOUT0        0x00  /* X[19:12] */
#define MMC5603_REG_XOUT1        0x01  /* X[11:4] */
#define MMC5603_REG_YOUT0        0x02  /* Y[19:12] */
#define MMC5603_REG_YOUT1        0x03  /* Y[11:4] */
#define MMC5603_REG_ZOUT0        0x04  /* Z[19:12] */
#define MMC5603_REG_ZOUT1        0x05  /* Z[11:4] */
#define MMC5603_REG_XOUT2        0x06  /* X[3:0] */
#define MMC5603_REG_YOUT2        0x07  /* Y[3:0] */
#define MMC5603_REG_ZOUT2        0x08  /* Z[3:0] */
#define MMC5603_REG_TOUT         0x09  /* Temperature output */

/* Status register */
#define MMC5603_REG_STATUS1      0x18
#define MMC5603_STATUS_OTP_DONE  0x10  /* Bit 4: OTP read done */
#define MMC5603_STATUS_MEAS_DONE 0x40  /* Bit 6: MM_DONE (measurement complete) */

/* Control registers */
#define MMC5603_REG_ODR          0x1A  /* Output data rate */
#define MMC5603_REG_CTRL0        0x1B  /* Internal control 0 */
#define MMC5603_REG_CTRL1        0x1C  /* Internal control 1 (BW) */
#define MMC5603_REG_CTRL2        0x1D  /* Internal control 2 */

/* CTRL0 bits */
#define MMC5603_CTRL0_TMM        0x01  /* Bit 0: Take measurement (self-clearing) */
#define MMC5603_CTRL0_SET        0x08  /* Bit 3: Do SET pulse (self-clearing) */
#define MMC5603_CTRL0_RESET      0x10  /* Bit 4: Do RESET pulse (self-clearing) */
#define MMC5603_CTRL0_AUTO_SR    0x20  /* Bit 5: Auto SET/RESET enable */

/* CTRL1 BW bits (bit[1:0]) */
#define MMC5603_CTRL1_BW00       0x00  /* 6.6 ms measurement time */
#define MMC5603_CTRL1_BW01       0x01  /* 3.5 ms */
#define MMC5603_CTRL1_BW10       0x02  /* 2.0 ms */
#define MMC5603_CTRL1_BW11       0x03  /* 1.2 ms */

/* Product ID register */
#define MMC5603_REG_PRODUCT_ID   0x39
#define MMC5603_PRODUCT_ID_VAL   0x10

/* Sensitivity: 16384 counts/G (20-bit mode) */
#define MMC5603_SENSITIVITY      16384.0f

/* Timing requirements */
#define MMC5603_POWERUP_DELAY_MS 5     /* Wait 5ms after power up */
#define MMC5603_SETRESET_DELAY_MS 1    /* Wait 1ms after SET/RESET */
