/**
 * ESP-CLAW LCD Display Driver (ST7735S 128x160)
 *
 * Uses pure GPIO bit-bang SPI instead of ESP-IDF hardware SPI DMA.
 * Root cause of previous failures: ESP32-S3 SPI.writeBytes() uses async DMA
 * that gets interrupted by subsequent operations, causing silent pixel data loss.
 *
 * Key architecture decisions:
 *   1. Continuous CS policy — CS held LOW during entire bulk pixel fill
 *   2. Bit-bang SPI at ~40kHz effective clock (delayMicroseconds(1))
 *   3. No DMA, no HAL SPI abstractions for pixel writes
 */

#include "lcd_display.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "lcd_display";

/* ===== Pin Definitions ===== */
#define PIN_MOSI      17
#define PIN_SCLK      18
#define PIN_CS        21
#define PIN_RST       15
#define PIN_DC        16

#define TFT_W         128
#define TFT_H         160
#define TOTAL_PIXELS  (TFT_W * TFT_H) /* 20480 */

/* ===== GPIO helpers ===== */

static inline void lcd_set_sclk(bool level) { gpio_set_level((gpio_num_t)PIN_SCLK, level); }
static inline void lcd_set_mosi(bool level)  { gpio_set_level((gpio_num_t)PIN_MOSI, level);  }
static inline void lcd_set_cs(bool level)    { gpio_set_level((gpio_num_t)PIN_CS, level);    }
static inline void lcd_set_dc(bool level)    { gpio_set_level((gpio_num_t)PIN_DC, level);    }
static inline void lcd_set_rst(bool level)   { gpio_set_level((gpio_num_t)PIN_RST, level);   }

/* ===== Bit-Bang SPI (~40kHz per byte) ===== */

static void bb_spi_byte_out(uint8_t d) {
    for (int i = 7; i >= 0; i--) {
        bool b = (d >> i) & 1;
        lcd_set_mosi(b ? true : false);
        delayMicroseconds(1);
        lcd_set_sclk(true);
        delayMicroseconds(1);
        lcd_set_sclk(false);
        delayMicroseconds(1);
    }
}

/* ===== LCD Command/Data Writers ===== */

static void lcd_cmd(uint8_t c) {
    lcd_set_cs(false);
    lcd_set_dc(false);
    bb_spi_byte_out(c);
    lcd_set_cs(true);
}

static void lcd_dat_one(uint8_t d) {
    lcd_set_cs(false);
    lcd_set_dc(true);
    bb_spi_byte_out(d);
    lcd_set_cs(true);
}

/*
 * Multi-byte data block attached to a command.
 * CRITICAL: CS stays LOW throughout command + data transaction.
 */
static void lcd_cmd_multi(uint8_t cmd, const uint8_t *data, int len) {
    lcd_set_cs(false);
    lcd_set_dc(false);
    bb_spi_byte_out(cmd);
    lcd_set_dc(true);
    for (int i = 0; i < len; i++) {
        bb_spi_byte_out(data[i]);
    }
    lcd_set_cs(true);
}

/* ===== Address Window Setup ===== */

static void set_addr_only(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    uint8_t col4[4] = {0, x0, 0, x1};
    lcd_cmd_multi(0x2A, col4, sizeof(col4));

    uint8_t row4[4] = {0, y0, 0, y1};
    lcd_cmd_multi(0x2B, row4, sizeof(row4));

    lcd_cmd(0x2C);
    vTaskDelay(pdMS_TO_TICKS(1)); /* Let LCD latch before filling */
}

/* ===== Full Screen Fill (Continuous CS Policy) ===== */

static volatile int s_fill_kbps = 0;

void lcd_fill_screen(uint16_t color) {
    set_addr_only(0, 0, TFT_W - 1, TFT_H - 1);

    uint8_t h = (color >> 8) & 0xFF;
    uint8_t l = color & 0xFF;

    /* Hold CS low for ENTIRE fill */
    lcd_set_cs(false);
    lcd_set_dc(true);

    for (int p = 0; p < TOTAL_PIXELS; p++) {
        bb_spi_byte_out(h);
        bb_spi_byte_out(l);
    }

    lcd_set_cs(true);

    /* Calculate speed */
    int bytes_transferred = TOTAL_PIXELS * 2;
    s_fill_kbps = (bytes_transferred / 1000); /* approximate */
}

/* ===== Address Window for Partial Area ===== */

void lcd_set_area(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    uint8_t col4[] = {(uint8_t)x0, (uint8_t)(x0 >> 8), (uint8_t)x1, (uint8_t)(x1 >> 8)};
    lcd_cmd_multi(0x2A, col4, sizeof(col4));

    uint8_t row4[] = {(uint8_t)y0, (uint8_t)(y0 >> 8), (uint8_t)y1, (uint8_t)(y1 >> 8)};
    lcd_cmd_multi(0x2B, row4, sizeof(row4));

    lcd_cmd(0x2C);
    vTaskDelay(pdMS_TO_TICKS(1));
}

void lcd_write_pixels(const uint16_t *pixels, int count) {
    lcd_set_cs(false);
    lcd_set_dc(true);

    for (int i = 0; i < count; i++) {
        uint16_t c = pixels[i];
        bb_spi_byte_out((c >> 8) & 0xFF);
        bb_spi_byte_out(c & 0xFF);
    }

    lcd_set_cs(true);
}

/* ===== Initialization Sequence (ST7735S-specific) ===== */

esp_err_t lcd_display_init(void) {
    /* Configure pins */
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(PIN_MOSI) | BIT64(PIN_SCLK) |
                        BIT64(PIN_CS)   | BIT64(PIN_RST) |
                        BIT64(PIN_DC),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Default states */
    lcd_set_cs(true);
    lcd_set_rst(true);
    lcd_set_dc(true);
    lcd_set_mosi(false);
    lcd_set_sclk(false);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Reset sequence */
    lcd_set_rst(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    lcd_set_rst(true);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "ST7735S init started");

    /* Core init commands */
    lcd_cmd(0x01);           /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x11);           /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x28);           /* DISP OFF first */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* PMCTRLA */
    uint8_t cbA[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
    lcd_cmd_multi(0xCB, cbA, sizeof(cbA));

    /* PMCTRLB */
    uint8_t cfB[] = {0x00, 0xC1, 0x30};
    lcd_cmd_multi(0xCF, cfB, sizeof(cfB));

    /* DRIVOUT */
    uint8_t b1a[] = {0x40, 0x82};
    lcd_cmd_multi(0xB1, b1a, sizeof(b1a));

    /* FRMRT1 */
    uint8_t b6b[] = {0x02, 0x02, 0x3B};
    lcd_cmd_multi(0xB6, b6b, sizeof(b6b));

    /* PWE2 */
    uint8_t edc[] = {0x64, 0x03, 0x12, 0x81};
    lcd_cmd_multi(0xED, edc, sizeof(edc));

    /* GAMMAEN */
    uint8_t e8d[] = {0x40, 0x54, 0x82, 0x00};
    lcd_cmd_multi(0xE8, e8d, sizeof(e8d));

    /* Gamma+ curves */
    uint8_t gp[] = {0x2F,0x24,0x1B,0x1C,0x1F,0x0B,0x34,0x61,0x4E,
                    0x23,0x08,0x13,0x0A,0x12,0x0E};
    lcd_cmd_multi(0xE0, gp, sizeof(gp));

    /* Gamma- curves */
    uint8_t gn[] = {0x2F,0x24,0x1B,0x1C,0x1F,0x0B,0x34,0x61,0x4E,
                    0x23,0x08,0x13,0x0A,0x12,0x0E};
    lcd_cmd_multi(0xE1, gn, sizeof(gn));

    /* Pixel format (RGB565) */
    uint8_t pf1[] = {0x55};
    lcd_cmd_multi(0x3A, pf1, sizeof(pf1));

    /* MADCTL (normal orientation) */
    uint8_t madctl[] = {0x00};
    lcd_cmd_multi(0x36, madctl, sizeof(madctl));

    /* Display ON sequence */
    lcd_cmd(0x20);       /* INV OFF */
    lcd_cmd(0x13);       /* NORON */
    lcd_cmd(0x29);       /* DISP ON */
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "ST7735S initialized successfully");
    return ESP_OK;
}

/* ===== Color Test Pattern ===== */

void lcd_run_color_test(void) {
    static const uint16_t colors[] = {
        0xFFFF, /* White */
        0x0000, /* Black */
        0xF800, /* Red    */
        0x07E0, /* Green  */
        0x001F, /* Blue   */
    };
    static const char* names[] = {"WHITE", "BLACK", "RED", "GREEN", "BLUE"};

    for (int pass = 0; pass < 2; pass++) {
        ESP_LOGI(TAG, "--- PASS %d ---", pass + 1);
        for (int i = 0; i < 5; i++) {
            ESP_LOGI(TAG, "FILL %s...", names[i]);
            lcd_fill_screen(colors[i]);
            vTaskDelay(pdMS_TO_TICKS(pass == 0 ? 2000 : 800));
        }
    }
    ESP_LOGI(TAG, "Color test complete!");
}

int lcd_get_fill_speed_kbps(void) {
    return s_fill_kbps;
}

void lcd_display_off(void) {
    lcd_cmd(0x28); /* Display OFF */
}
