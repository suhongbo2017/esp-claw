/**
 * ST7735S SPI LCD Test Firmware (Standalone)
 * 
 * Hardware: ESP32-S3 + ST7735S 1.44" 128x160 TFT
 * Interface: SPI @ 40MHz, RGB565 pixel format
 * 
 * Pin Mapping:
 *   CS:  GPIO21
 *   MOSI:GPIO17  
 *   CLK: GPIO18
 *   DC:  GPIO16
 *   RST: GPIO15
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "LCD_TEST";

// ── Pin definitions from schematic ──────────────────────────────────────
#define PIN_LCD_CS    21
#define PIN_LCD_MOSI  17
#define PIN_LCD_CLK   18
#define PIN_LCD_DC    16
#define PIN_LCD_RST   15

// ── Display specs ───────────────────────────────────────────────────────
#define TFT_WIDTH     128
#define TFT_HEIGHT    160

// ── Color helpers (RGB565) ─────────────────────────────────────────────
typedef uint16_t rgb565;

#define RGB565(r,g,b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

#define COLOR_RED      RGB565(255, 0, 0)
#define COLOR_GREEN    RGB565(0, 255, 0)
#define COLOR_BLUE     RGB565(0, 0, 255)
#define COLOR_YELLOW   RGB565(255, 255, 0)
#define COLOR_CYAN     RGB565(0, 255, 255)
#define COLOR_MAGENTA  RGB565(255, 0, 255)
#define COLOR_WHITE    RGB565(255, 255, 255)
#define COLOR_BLACK    RGB565(0, 0, 0)

// ── SPI handle ──────────────────────────────────────────────────────────
static spi_device_handle_t lcd_spi;

// ── Helper: write command ──────────────────────────────────────────────
static inline void lcd_cmd(uint8_t cmd) {
    gpio_set_level(PIN_LCD_DC, 0);
    gpio_set_level(PIN_LCD_CS, 0);
    
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_transmit(lcd_spi, &t);
    
    gpio_set_level(PIN_LCD_CS, 1);
}

// ── Helper: write data ─────────────────────────────────────────────────
static inline void lcd_data(const uint8_t *data, size_t len) {
    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);
    
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_transmit(lcd_spi, &t);
    
    gpio_set_level(PIN_LCD_CS, 1);
}

// ── Hardware reset ─────────────────────────────────────────────────────
static void lcd_reset(void) {
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));  // Power on stabilization
}

// ── ST7735S initialization sequence ───────────────────────────────────
static void lcd_init(void) {
    ESP_LOGI(TAG, "Initializing ST7735S...");
    
    // Send software reset
    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // Sleep out
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    
    // Frame rate control (normal mode)
    lcd_data((uint8_t[]){0x00, 0x1B}, 2);
    lcd_cmd(0xB1);
    
    // Display inversion control
    lcd_cmd(0xB3);
    lcd_data((uint8_t[]){0x03}, 1);
    
    // Power control 1
    lcd_cmd(0xC0);
    lcd_data((uint8_t[]){0xA2, 0x02, 0x84}, 3);
    
    // Power control 2
    lcd_cmd(0xC1);
    lcd_data((uint8_t[]){0x0C}, 1);
    
    // VCOM control
    lcd_cmd(0xC5);
    lcd_data((uint8_t[]){0x0D}, 1);
    
    // Column address set (start=0, end=127)
    lcd_cmd(0x2A);
    lcd_data((uint8_t[]){0x00, 0x00, 0x00, 0x7F}, 4);
    
    // Page address set (start=0, end=159)
    lcd_cmd(0x2B);
    lcd_data((uint8_t[]){0x00, 0x00, 0x00, 0x9F}, 4);
    
    // Pixel format (16-bit RGB565)
    lcd_cmd(0x3A);
    lcd_data((uint8_t[]){0x05}, 1);
    
    // Memory access control (MADCTL) - portrait orientation
    uint8_t madctl = 0x20;  // MY=1 for portrait
    lcd_cmd(0x36);
    lcd_data(&madctl, 1);
    
    // Display on
    lcd_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "✓ ST7735S initialized!");
}

// ── Fill screen with solid color ───────────────────────────────────────
static void fill_color(rgb565 color) {
    size_t pixels = TFT_WIDTH * TFT_HEIGHT;
    size_t batch_pixels = 64;
    int batch_bytes = batch_pixels * 2;
    
    uint8_t buf[batch_bytes];
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    
    for (int i = 0; i < batch_bytes; i += 2) {
        buf[i] = hi;
        buf[i+1] = lo;
    }
    
    lcd_cmd(0x2C);  // Memory write
    
    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);
    
    for (size_t remaining = pixels; remaining > 0; remaining -= batch_pixels) {
        int send_pixels = (remaining > batch_pixels) ? batch_pixels : remaining;
        
        spi_transaction_t t = {
            .length = send_pixels * 16,
            .tx_buffer = buf,
        };
        spi_device_transmit(lcd_spi, &t);
    }
    
    gpio_set_level(PIN_LCD_CS, 1);
}

// ── Show solid color with label delay ──────────────────────────────────
static void show_solid(rgb565 color, const char *name) {
    ESP_LOGI(TAG, "[%s]", name);
    fill_color(color);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ── Color bars pattern ────────────────────────────────────────────────
static void show_bars(void) {
    ESP_LOGI(TAG, "[Color Bars]");
    
    rgb565 colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW,
        COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, COLOR_BLACK
    };
    int count = sizeof(colors) / sizeof(colors[0]);
    int bar_h = TFT_HEIGHT / count;
    
    lcd_cmd(0x2C);
    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);
    
    for (int row = 0; row < count; row++) {
        uint8_t hi = (colors[row] >> 8) & 0xFF;
        uint8_t lo = colors[row] & 0xFF;
        uint8_t pat[8];
        for (int i = 0; i < 8; i += 2) {
            pat[i] = hi;
            pat[i+1] = lo;
        }
        
        // Draw this bar row by row to avoid huge DMA transfers
        for (int y = 0; y < bar_h; y++) {
            for (int x = 0; x < TFT_WIDTH; x += 4) {
                spi_transaction_t t = {
                    .length = 4 * 16,
                    .tx_buffer = pat,
                };
                spi_device_transmit(lcd_spi, &t);
            }
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  ST7735S LCD TEST - Standalone");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Hardware config:");
    ESP_LOGI(TAG, "  Chip:       ESP32-S3");
    ESP_LOGI(TAG, "  Screen:     ST7735S 128x160 RGB565");
    ESP_LOGI(TAG, "  Interface:  SPI @ 40MHz");
    ESP_LOGI(TAG, "  Pins:");
    ESP_LOGI(TAG, "    CS=%d  MOSI=%d  CLK=%d", 
             PIN_LCD_CS, PIN_LCD_MOSI, PIN_LCD_CLK);
    ESP_LOGI(TAG, "    DC=%d  RST=%d", PIN_LCD_DC, PIN_LCD_RST);
    
    // ── Initialize GPIOs ─────────────────────────────────────────────
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_DC) |
                        (1ULL << PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);
    
    // Initial states
    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_LCD_DC, 0);
    gpio_set_level(PIN_LCD_RST, 1);
    
    // ── Initialize SPI bus ───────────────────────────────────────────
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = PIN_LCD_MOSI,
        .miso_io_num   = -1,          // LCD only uses MOSI
        .sclk_io_num   = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * 10,
    };
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SPI: %s", esp_err_to_name(ret));
        return;
    }
    
    // Add LCD device (we use software CS)
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,       // Use manual CS via GPIO
        .queue_size = 7,
    };
    
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &lcd_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add LCD device: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "SPI initialized at 40MHz\n");
    
    // ── Hardware reset ───────────────────────────────────────────────
    lcd_reset();
    
    // ── Software initialization ──────────────────────────────────────
    lcd_init();
    
    // ── Display test sequence ────────────────────────────────────────
    ESP_LOGI(TAG, "\n=== DISPLAY TEST ===\n");
    
    // Solid colors (1s each)
    show_solid(COLOR_RED, "RED");
    show_solid(COLOR_GREEN, "GREEN");
    show_solid(COLOR_BLUE, "BLUE");
    
    // Color bars
    show_bars();
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    // More solids
    show_solid(COLOR_YELLOW, "YELLOW");
    show_solid(COLOR_CYAN, "CYAN");
    show_solid(COLOR_MAGENTA, "MAGENTA");
    show_solid(COLOR_WHITE, "WHITE");
    show_solid(COLOR_BLACK, "BLACK");
    
    ESP_LOGI(TAG, "\n=== STARTING FLASH LOOP ===\n");
    
    // Flashing loop
    rgb565 flash_colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA
    };
    int num_colors = sizeof(flash_colors) / sizeof(flash_colors[0]);
    
    while (1) {
        for (int i = 0; i < num_colors; i++) {
            fill_color(flash_colors[i]);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
}
