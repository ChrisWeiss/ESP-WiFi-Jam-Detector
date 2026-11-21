/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Board bring-up for the Waveshare ESP32-C5-Touch-LCD-2.8: I2C on GPIO0/1,
 * then the CH32V003 expander pulses touch/LCD reset, then the ST7789 is set
 * up over SPI2. The audio PA (EXIO3) stays off until a beep. Battery voltage
 * comes from the expander's ADC (register 0x06), not an AXP2101.
 */
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "board.h"
#include "board_pins.h"
#include "st7789.h"

static const char *TAG = "board";

#define I2C_TIMEOUT_MS   100
#define LCD_MAX_TRANSFER (BOARD_LCD_W * 40 * 2)

/* CST3530 reports one point in the first 9 bytes at BOARD_TOUCH_DATA_REG.
 * Layout matches Waveshare's esp_lcd_touch_cst3530.c. */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exio, s_touch;
static uint8_t s_exio_out;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lcd_done;
/* "Radio quiet" token: held for the whole of each LCD transfer and by the
 * sweep for each capture, so LCD DMA never runs while the receiver samples.
 * Measured in M3: LCD traffic adds 4-9 dB spurs and up to +3.4 dB floor at
 * 2.4 GHz (none at 5 GHz). Given back from the transfer-done ISR. */
static SemaphoreHandle_t s_rf_token;
/* Captures take precedence: while one waits, no new LCD transfer starts, so a
 * capture waits for at most the transfer already in flight. Without this the
 * higher-priority UI re-took the token after every strip and starved it. */
static atomic_int s_rf_waiting; /* atomic: the sweep and the event log both count in; a lost
                                   update once left it at -1 and stalled every LCD strip */
static SemaphoreHandle_t s_rf_done;
static board_lcd_done_cb_t s_async_cb;
static void *s_async_ctx;
static board_orient_t s_orient;

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    return i2c_master_transmit(dev, (uint8_t[]) {reg, val}, 2, I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t add_dev(uint16_t addr, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, dev);
}

static esp_err_t exio_set(uint8_t mask, bool level)
{
    uint8_t next = level ? (s_exio_out | mask) : (s_exio_out & ~mask);
    ESP_RETURN_ON_ERROR(reg_write(s_exio, BOARD_EXIO_REG_OUT, next), TAG, "expander output write failed");
    s_exio_out = next;
    return ESP_OK;
}

static esp_err_t exio_init(void)
{
    const uint8_t outputs = BIT(BOARD_EXIO_TP_RST) | BIT(BOARD_EXIO_LCD_RST) | BIT(BOARD_EXIO_PA_EN);

    ESP_RETURN_ON_ERROR(add_dev(BOARD_EXIO_ADDR, &s_exio), TAG, "add expander failed");
    /* Outputs low before switching direction, so nothing glitches high. */
    s_exio_out = 0;
    ESP_RETURN_ON_ERROR(reg_write(s_exio, BOARD_EXIO_REG_OUT, 0), TAG, "expander not responding at 0x%02x",
                        BOARD_EXIO_ADDR);
    ESP_RETURN_ON_ERROR(reg_write(s_exio, BOARD_EXIO_REG_PWM, 0), TAG, "backlight off failed");
    ESP_RETURN_ON_ERROR(reg_write(s_exio, BOARD_EXIO_REG_DIR, outputs), TAG, "expander direction failed");

    /* Reset pulse as in BSP bsp_io_expander_init(): high 50 ms, low 50 ms, then
     * release both resets and raise P5. */
    ESP_RETURN_ON_ERROR(exio_set(BIT(BOARD_EXIO_TP_RST) | BIT(BOARD_EXIO_LCD_RST), true), TAG, "reset high");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(exio_set(BIT(BOARD_EXIO_TP_RST) | BIT(BOARD_EXIO_LCD_RST), false), TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(exio_set(BIT(BOARD_EXIO_TP_RST) | BIT(BOARD_EXIO_LCD_RST), true), TAG, "reset release");
    vTaskDelay(pdMS_TO_TICKS(200)); /* CST3530 needs longer than the ST7789's 120 ms */
    ESP_LOGI(TAG, "expander ok: dir=0x%02x out=0x%02x", outputs, s_exio_out);
    return ESP_OK;
}

esp_err_t board_i2c_scan(void)
{
    char line[160] = "";
    size_t n = 0;
    for (uint16_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_bus, a, 20) == ESP_OK) {
            n += snprintf(line + n, sizeof(line) - n, " 0x%02x", a);
            if (n >= sizeof(line)) {
                break;
            }
        }
    }
    ESP_LOGI(TAG, "i2c devices:%s", n ? line : " none");
    return ESP_OK;
}

/* One sample. Waveshare averages eight; a single read is enough for the status line.
 * 3.3 V reference and a 3:1 divider. */
static esp_err_t battery_mv(uint16_t *mv)
{
    uint8_t raw[2];
    ESP_RETURN_ON_ERROR(reg_read(s_exio, BOARD_EXIO_REG_ADC, raw, sizeof(raw)), TAG, "battery ADC read failed");
    const uint16_t sample = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
    ESP_RETURN_ON_FALSE(sample <= 1023, ESP_ERR_INVALID_RESPONSE, TAG, "battery ADC sample %u", sample);
    *mv = (uint16_t)(sample * 3300u / 1023u * 3u);
    return ESP_OK;
}

esp_err_t board_pmu_log(void)
{
    uint16_t mv = 0;
    ESP_RETURN_ON_ERROR(battery_mv(&mv), TAG, "battery");
    ESP_LOGI(TAG, "battery %.2f V (CH32 ADC)", mv / 1000.0);
    return ESP_OK;
}

esp_err_t board_power_read(board_power_t *out)
{
    uint16_t mv = 0;
    ESP_RETURN_ON_ERROR(battery_mv(&mv), TAG, "battery");
    *out = (board_power_t){
        .battery = true,
        .percent = -1, /* the CH32 ADC has no fuel gauge */
        .battery_mv = mv,
    };
    return ESP_OK;
}

esp_err_t board_pa_enable(bool on)
{
    return exio_set(BIT(BOARD_EXIO_PA_EN), on);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

esp_err_t board_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return reg_write(s_exio, BOARD_EXIO_REG_PWM, (uint8_t)(percent * 255u / 100u));
}

/* Runs in the SPI ISR. CONFIG_SPI_MASTER_ISR_IN_IRAM is off (saves IRAM), so
 * this and the async callback may live in flash. */
static bool lcd_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    board_lcd_done_cb_t cb = s_async_cb;
    void *cb_ctx = s_async_ctx;
    s_async_cb = NULL;
    xSemaphoreGiveFromISR(s_rf_token, &woken);
    if (cb) {
        cb(cb_ctx);
    } else {
        xSemaphoreGiveFromISR(s_lcd_done, &woken);
    }
    return woken == pdTRUE;
}

esp_err_t board_lcd_set_orient(board_orient_t orient)
{
    /* Portrait MADCTL is 0x00. MV alone exchanges the axes and leaves the
     * picture mirrored. MX|MV is the readable 90-degree view; MY|MV turns
     * that view 180 degrees. board_touch_read uses the same two mappings. */
    bool flip = orient == BOARD_ORIENT_LANDSCAPE_FLIP;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, !flip, flip), TAG, "mirror failed");
    s_orient = orient;
    return ESP_OK;
}

board_orient_t board_lcd_orient(void)
{
    return s_orient;
}

static esp_err_t lcd_init(board_orient_t orient)
{
    s_lcd_done = xSemaphoreCreateBinary();
    s_rf_token = xSemaphoreCreateBinary();
    s_rf_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_lcd_done && s_rf_token && s_rf_done, ESP_ERR_NO_MEM, TAG, "no mem for LCD semaphores");
    xSemaphoreGive(s_rf_token);

    /* Keep the TF card deselected while the LCD uses the shared bus. Not
     * gpio_config(): that reserves the pin, and the SDSPI driver takes it
     * over later (main/eventlog.c). */
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_SD_CS, 1), TAG, "SD CS high failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(BOARD_SD_CS, GPIO_MODE_OUTPUT), TAG, "SD CS config failed");

    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = BOARD_SD_MISO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_MAX_TRANSFER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = lcd_trans_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.psram_dma_direct = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_lcd_io),
                        TAG, "panel IO init failed");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, /* hardware reset already done via expander P1 */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(st7789_new_panel(s_lcd_io, &panel_cfg, &s_panel), TAG, "ST7789 create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "ST7789 reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "ST7789 init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(board_lcd_set_orient(orient), TAG, "orientation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");
    ESP_LOGI(TAG, "ST7789 ok, %dx%d landscape, orient=%d", BOARD_LCD_W, BOARD_LCD_H, orient);
    return ESP_OK;
}

esp_err_t board_lcd_draw_async(int x0, int y0, int x1, int y1, const void *px, board_lcd_done_cb_t done, void *ctx)
{
    for (int i = 0; atomic_load(&s_rf_waiting) > 0 && i < 50; i++) {
        xSemaphoreTake(s_rf_done, pdMS_TO_TICKS(10)); /* let the pending capture go first */
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_rf_token, pdMS_TO_TICKS(500)) == pdTRUE, ESP_ERR_TIMEOUT, TAG,
                        "LCD bus busy");
    s_async_ctx = ctx;
    s_async_cb = done;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, px);
    if (err != ESP_OK) {
        s_async_cb = NULL;
        xSemaphoreGive(s_rf_token);
        ESP_LOGE(TAG, "draw failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t board_lcd_draw(int x0, int y0, int x1, int y1, const void *px)
{
    ESP_RETURN_ON_ERROR(board_lcd_draw_async(x0, y0, x1, y1, px, NULL, NULL), TAG, "draw");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lcd_done, pdMS_TO_TICKS(500)) == pdTRUE, ESP_ERR_TIMEOUT, TAG,
                        "LCD transfer timeout");
    return ESP_OK;
}

esp_err_t board_rf_quiet_begin(uint32_t timeout_ms)
{
    atomic_fetch_add(&s_rf_waiting, 1);
    bool ok = xSemaphoreTake(s_rf_token, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    atomic_fetch_sub(&s_rf_waiting, 1);
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

void board_rf_quiet_end(void)
{
    xSemaphoreGive(s_rf_token);
    xSemaphoreGive(s_rf_done);
}

static esp_err_t touch_cmd(uint32_t reg, uint8_t *buf, size_t len)
{
    const uint8_t addr[4] = {reg >> 24, reg >> 16, reg >> 8, reg};
    if (len == 0) {
        return i2c_master_transmit(s_touch, addr, sizeof(addr), I2C_TIMEOUT_MS);
    }
    return i2c_master_transmit_receive(s_touch, addr, sizeof(addr), buf, len, I2C_TIMEOUT_MS);
}

esp_err_t board_touch_read(board_touch_t *t)
{
    uint8_t d[9];
    ESP_RETURN_ON_FALSE(s_touch, ESP_ERR_INVALID_STATE, TAG, "touch not initialised");
    ESP_RETURN_ON_ERROR(touch_cmd(BOARD_TOUCH_DATA_REG, d, sizeof(d)), TAG, "touch read failed");
    const uint8_t points = d[3] & 0x0F;
    t->pressed = points > 0 && points <= 2 && (d[8] & 0xF0) != 0;
    esp_err_t end = touch_cmd(BOARD_TOUCH_END_REG, NULL, 0);
    if (end != ESP_OK) {
        ESP_LOGW(TAG, "touch end-read: %s", esp_err_to_name(end));
    }
    if (!t->pressed) {
        return ESP_OK;
    }
    uint16_t tx = (uint16_t)(((d[7] & 0x0F) << 8) | d[4]);
    uint16_t ty = (uint16_t)(((d[7] & 0xF0) << 4) | d[5]);
    if (tx >= BOARD_LCD_NATIVE_W) {
        tx = BOARD_LCD_NATIVE_W - 1;
    }
    if (ty >= BOARD_LCD_NATIVE_H) {
        ty = BOARD_LCD_NATIVE_H - 1;
    }
    t->raw_x = tx;
    t->raw_y = ty;
    /* CST3530 reports portrait scan coordinates. MX|MV maps landscape
     * (x, y) onto portrait (y, native_w - 1 - x); MY|MV is the 180 turn. */
    if (s_orient == BOARD_ORIENT_LANDSCAPE) {
        t->x = ty;
        t->y = (uint16_t)(BOARD_LCD_NATIVE_W - 1 - tx);
    } else {
        t->x = (uint16_t)(BOARD_LCD_NATIVE_H - 1 - ty);
        t->y = tx;
    }
    return ESP_OK;
}

bool board_boot_pressed(void)
{
    return gpio_get_level(BOARD_BOOT_BTN) == 0;
}

esp_err_t board_init(board_orient_t orient)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus init failed");
    ESP_RETURN_ON_ERROR(exio_init(), TAG, "IO expander init failed");
    ESP_RETURN_ON_ERROR(board_i2c_scan(), TAG, "I2C scan failed");

    esp_err_t err = board_pmu_log();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC unavailable: %s", esp_err_to_name(err));
    }

    const gpio_config_t inputs = {
        .pin_bit_mask = BIT64(BOARD_BOOT_BTN) | BIT64(BOARD_TOUCH_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&inputs), TAG, "button/touch INT config failed");

    ESP_RETURN_ON_ERROR(lcd_init(orient), TAG, "LCD init failed");

    /* CST3530 was reset with the LCD. A failed probe must not skip the backlight:
     * the screen still runs, and touches are reported as released. */
    ESP_RETURN_ON_ERROR(add_dev(BOARD_TOUCH_ADDR, &s_touch), TAG, "add touch failed");
    board_touch_t t;
    err = board_touch_read(&t);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(300));
        err = board_touch_read(&t);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST3530 not responding at 0x%02x: %s", BOARD_TOUCH_ADDR, esp_err_to_name(err));
        s_touch = NULL;
    } else {
        ESP_LOGI(TAG, "CST3530 ok");
    }
    return ESP_OK;
}
