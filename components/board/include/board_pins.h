/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Waveshare ESP32-C5-Touch-LCD-2.8 pin and bus map.
 *
 * Values are from Waveshare's ESP32-C5-Touch-LCD-2.8 resource repository
 * (examples/esp-idf/01_lvgl_demo BSP header and examples/arduino/libraries/
 * board_config Board_IO.h / Display_ST7789.h).
 */
#pragma once

#include "driver/gpio.h"

/* Shared I2C: CH32V003, CST3530, ES8311, RTC, IMU, SHTC3. */
#define BOARD_I2C_PORT        0
#define BOARD_I2C_SDA         GPIO_NUM_0
#define BOARD_I2C_SCL         GPIO_NUM_1
#define BOARD_I2C_HZ          400000

/* CH32V003 IO expander. Registers match Waveshare's 8-bit protocol. */
#define BOARD_EXIO_ADDR       0x24
#define BOARD_EXIO_REG_DIR    0x02 /* bit = 1 -> output */
#define BOARD_EXIO_REG_OUT    0x03
#define BOARD_EXIO_REG_IN     0x04
#define BOARD_EXIO_REG_PWM    0x05 /* raw 0..255, backlight */
#define BOARD_EXIO_REG_ADC    0x06 /* battery, uint16 LE, 10-bit */
#define BOARD_EXIO_TP_RST     0
#define BOARD_EXIO_LCD_RST    1
#define BOARD_EXIO_PA_EN      3 /* NS4150B enable */

/* ST7789 on SPI2, shared with the TF card. Native panel is portrait. */
#define BOARD_LCD_SPI_HOST    SPI2_HOST
#define BOARD_LCD_PCLK_HZ     (40 * 1000 * 1000)
#define BOARD_LCD_SCLK        GPIO_NUM_6
#define BOARD_LCD_MOSI        GPIO_NUM_7
#define BOARD_LCD_DC          GPIO_NUM_9
#define BOARD_LCD_CS          GPIO_NUM_10
#define BOARD_LCD_NATIVE_W    240
#define BOARD_LCD_NATIVE_H    320

#define BOARD_SD_CS           GPIO_NUM_23
#define BOARD_SD_MISO         GPIO_NUM_8

/* CST3530 touch. Register access uses a 32-bit address. */
#define BOARD_TOUCH_ADDR      0x58
#define BOARD_TOUCH_INT       GPIO_NUM_5
#define BOARD_TOUCH_DATA_REG  0xD0070000u
#define BOARD_TOUCH_END_REG   0xD00002ABu

#define BOARD_BOOT_BTN        GPIO_NUM_28 /* active low */

/* ES8311 full-duplex I2S. Playback uses BCLK/WS/DOUT; DIN is unused here. */
#define BOARD_I2S_BCLK        GPIO_NUM_24
#define BOARD_I2S_MCLK        GPIO_NUM_NC
#define BOARD_I2S_WS          GPIO_NUM_25
#define BOARD_I2S_DOUT        GPIO_NUM_26
#define BOARD_I2S_DIN         GPIO_NUM_27
