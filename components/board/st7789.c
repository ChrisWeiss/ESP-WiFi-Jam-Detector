/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Panel operations follow espressif/esp_lcd_st7796 (hardware reset is done by
 * the board through the IO expander). The vendor command list is Waveshare's
 * ESP32-C5-Touch-LCD-2.8 sequence (Display_ST7789.cpp), minus the commands
 * this driver sends itself: SLPOUT, MADCTL, COLMOD, INVON and DISPON.
 * Distributed as part of a GPL-3.0-or-later program; Apache-2.0 is compatible
 * with GPLv3.
 */
#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_check.h"
#include "esp_log.h"
#include "st7789.h"

static const char *TAG = "st7789";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
} st7789_panel_t;

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes; /* 0xFF = end of list */
} lcd_init_cmd_t;

/* Waveshare Display_ST7789.cpp LCD_Init(), excluding 0x11/0x36/0x3A/0x21/0x29. */
static const lcd_init_cmd_t vendor_specific_init[] = {
    {0xB0, {0x00, 0xE8}, 2},
    {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x75}, 1},
    {0xBB, {0x1A}, 1},
    {0xC0, {0x2C}, 1},
    {0xC2, {0x01, 0xFF}, 2},
    {0xC3, {0x13}, 1},
    {0xC4, {0x20}, 1},
    {0xC6, {0x0F}, 1},
    {0xD0, {0xA4, 0xA1}, 2},
    {0xD6, {0xA1}, 1},
    {0xE0, {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30}, 14},
    {0xE1, {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E}, 14},
    {0, {0}, 0xff},
};

static esp_err_t write_madctl(st7789_panel_t *st)
{
    return esp_lcd_panel_io_tx_param(st->io, LCD_CMD_MADCTL, (uint8_t[]) {st->madctl_val}, 1);
}

static esp_err_t panel_st7789_del(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    free(st);
    return ESP_OK;
}

static esp_err_t panel_st7789_reset(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st->io, LCD_CMD_SWRESET, NULL, 0), TAG, "SWRESET failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t panel_st7789_init(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st->io;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(write_madctl(st), TAG, "MADCTL failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {st->colmod_val}, 1), TAG,
                        "COLMOD failed");
    for (int i = 0; vendor_specific_init[i].data_bytes != 0xff; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, vendor_specific_init[i].cmd, vendor_specific_init[i].data,
                                                      vendor_specific_init[i].data_bytes & 0x1F),
                            TAG, "vendor init cmd 0x%02x failed", vendor_specific_init[i].cmd);
    }
    return ESP_OK;
}

static esp_err_t panel_st7789_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG, "empty area");
    esp_lcd_panel_io_handle_t io = st->io;

    x_start += st->x_gap;
    x_end += st->x_gap;
    y_start += st->y_gap;
    y_end += st->y_gap;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF, x_start & 0xFF, ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF, y_start & 0xFF, ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "RASET failed");
    size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * st->fb_bits_per_pixel / 8;
    return esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);
}

static esp_err_t panel_st7789_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    return esp_lcd_panel_io_tx_param(st->io, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_st7789_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    st->madctl_val = mirror_x ? (st->madctl_val | LCD_CMD_MX_BIT) : (st->madctl_val & ~LCD_CMD_MX_BIT);
    st->madctl_val = mirror_y ? (st->madctl_val | LCD_CMD_MY_BIT) : (st->madctl_val & ~LCD_CMD_MY_BIT);
    return write_madctl(st);
}

static esp_err_t panel_st7789_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    st->madctl_val = swap_axes ? (st->madctl_val | LCD_CMD_MV_BIT) : (st->madctl_val & ~LCD_CMD_MV_BIT);
    return write_madctl(st);
}

static esp_err_t panel_st7789_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    st->x_gap = x_gap;
    st->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7789_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    st7789_panel_t *st = __containerof(panel, st7789_panel_t, base);
    return esp_lcd_panel_io_tx_param(st->io, on ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

esp_err_t st7789_new_panel(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *cfg,
                           esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && cfg && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->reset_gpio_num < 0, ESP_ERR_NOT_SUPPORTED, TAG,
                        "reset is driven by the IO expander, not a GPIO");

    uint8_t madctl = 0, colmod = 0, fb_bpp = 0;
    switch (cfg->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        madctl |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported element order");
    }
    switch (cfg->bits_per_pixel) {
    case 16: /* RGB565. This panel's Waveshare init uses COLMOD 0x05, not 0x55. */
        colmod = 0x05;
        fb_bpp = 16;
        break;
    default:
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported pixel width");
    }

    st7789_panel_t *st = calloc(1, sizeof(st7789_panel_t));
    ESP_RETURN_ON_FALSE(st, ESP_ERR_NO_MEM, TAG, "no mem for st7789 panel");
    st->io = io;
    st->madctl_val = madctl;
    st->colmod_val = colmod;
    st->fb_bits_per_pixel = fb_bpp;
    st->base.del = panel_st7789_del;
    st->base.reset = panel_st7789_reset;
    st->base.init = panel_st7789_init;
    st->base.draw_bitmap = panel_st7789_draw_bitmap;
    st->base.invert_color = panel_st7789_invert_color;
    st->base.set_gap = panel_st7789_set_gap;
    st->base.mirror = panel_st7789_mirror;
    st->base.swap_xy = panel_st7789_swap_xy;
    st->base.disp_on_off = panel_st7789_disp_on_off;
    *ret_panel = &st->base;
    ESP_LOGD(TAG, "new st7789 panel @%p", st);
    return ESP_OK;
}
