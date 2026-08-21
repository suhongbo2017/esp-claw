/**
 * ESP-Claw COM2 Board - ST7735S LCD Test (Final)
 * 
 * Pin Definitions (from schematic):
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

// Pin definitions from schematic
#define PIN_LCD_CS    21
#define PIN_LCD_MOSI  17
#define PIN_LCD_CLK   18
#define PIN_LCD_DC    16
#define PIN_LCD_RST   15

// Screen specs
#define TFT_WIDTH     128
#define TFT_HEIGHT    160

// Color definitions (RGB565)
#define COLOR_RED      0xF800
#define COLOR_GREEN    0x07E0
#define COLOR_BLUE     0x001F
#define COLOR_YELLOW   0xFFE0
#define COLOR_CYAN     0x07FF
#define COLOR_MAGENTA  0xF81F
#define COLOR_WHITE    0xFFFF
#define COLOR_BLACK    0x0000

// Global SPI handle
static spi_device_handle_t lcd_spi;

/**
 * Initialize SPI bus for LCD
 */
void lcd_spi_init(void) {
    ESP_LOGI(TAG, "Initializing SPI bus...");
    
    // Initialize SPI bus (HSPI/SPI2)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,  // LCD only uses MOSI
        .sclk_io_num = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SPI bus: %s", esp_err_to_name(ret));
        return;
    }
    
    // Add LCD device
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,  // 40MHz
        .mode = 0,                            // CPOL=0, CPHA=0
        .spics_io_num = -1,                   // We control CS manually via GPIO
        .queue_size = 7,
    };
    
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &lcd_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add LCD device: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "SPI initialized at 40MHz");
}

/**
 * Initialize GPIO pins
 */
void lcd_gpio_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_DC) | 
                        (1ULL << PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // Default states
    gpio_set_level(PIN_LCD_CS, 1);  // Deselect
    gpio_set_level(PIN_LCD_DC, 0);  // Command mode
    gpio_set_level(PIN_LCD_RST, 1); // Normal operation
}

/**
 * Hardware reset sequence
 */
void lcd_reset(void) {
    ESP_LOGI(TAG, "Resetting LCD...");
    
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));  // Wait for power-on
}

/**
 * Write command to LCD
 */
static void lcd_write_cmd(uint8_t cmd) {
    gpio_set_level(PIN_LCD_DC, 0);  // CMD mode
    gpio_set_level(PIN_LCD_CS, 0);  // Select
    
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_transmit(lcd_spi, &t);
    
    gpio_set_level(PIN_LCD_CS, 1);  // Deselect
}

/**
 * Write data to LCD
 */
static void lcd_write_data(const uint8_t *data, size_t len) {
    gpio_set_level(PIN_LCD_DC, 1);  // DATA mode
    gpio_set_level(PIN_LCD_CS, 0);  // Select
    
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_transmit(lcd_spi, &t);
    
    gpio_set_level(PIN_LCD_CS, 1);  // Deselect
}

/**
 * Set display window for writing
 */
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t buf[4];
    
    // Column address set (0x2A)
    buf[0] = (x0 >> 8) & 0xFF;
    buf[1] = x0 & 0xFF;
    buf[2] = (x1 >> 8) & 0xFF;
    buf[3] = x1 & 0xFF;
    lcd_write_cmd(0x2A);
    lcd_write_data(buf, 4);
    
    // Row address set (0x2B)
    buf[0] = (y0 >> 8) & 0xFF;
    buf[1] = y0 & 0xFF;
    buf[2] = (y1 >> 8) & 0xFF;
    buf[3] = y1 & 0xFF;
    lcd_write_cmd(0x2B);
    lcd_write_data(buf, 4);
    
    // Memory write (0x2C)
    lcd_write_cmd(0x2C);
}

/**
 * Fill screen with single color
 */
static void lcd_fill(uint16_t color) {
    lcd_set_window(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    
    gpio_set_level(PIN_LCD_DC, 1);  // DATA mode
    gpio_set_level(PIN_LCD_CS, 0);  // Select
    
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    
    // Use buffer for faster transfer
    uint8_t buf[64];
    for (int i = 0; i < 64; i += 2) {
        buf[i] = hi;
        buf[i+1] = lo;
    }
    
    int pixels = TFT_WIDTH * TFT_HEIGHT;
    int batch = 32;  // 32 pixels per transaction
    
    for (int remaining = pixels; remaining > 0; remaining -= batch) {
        int transfer_pixels = (remaining > batch) ? batch : remaining;
        
        spi_transaction_t t = {
            .length = transfer_pixels * 16,
            .tx_buffer = buf,
        };
        spi_device_transmit(lcd_spi, &t);
    }
    
    gpio_set_level(PIN_LCD_CS, 1);  // Deselect
}

/**
 * Show solid color for 1 second
 */
static void show_solid(uint16_t color, const char *name) {
    ESP_LOGI(TAG, "[%s]", name);
    lcd_fill(color);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

/**
 * Show color bars pattern
 */
static void show_color_bars(void) {
    ESP_LOGI(TAG, "[Color Bars]");
    
    uint16_t colors[] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW, 
                         COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, COLOR_BLACK};
    int count = sizeof(colors) / sizeof(colors[0]);
    int bar_h = TFT_HEIGHT / count;
    
    for (int i = 0; i < count; i++) {
        lcd_set_window(0, i * bar_h, TFT_WIDTH - 1, (i + 1) * bar_h - 1);
        
        gpio_set_level(PIN_LCD_DC, 1);
        gpio_set_level(PIN_LCD_CS, 0);
        
        uint8_t hi = colors[i] >> 8;
        uint8_t lo = colors[i] & 0xFF;
        uint8_t buf[16];
        for (int j = 0; j < 16; j += 2) {
            buf[j] = hi;
            buf[j+1] = lo;
        }
        
        int pixels = TFT_WIDTH * bar_h;
        for (int p = 0; p < pixels; p += 8) {
            spi_transaction_t t = {
                .length = 8 * 16,
                .tx_buffer = buf,
            };
            spi_device_transmit(lcd_spi, &t);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  COM2 Board ST7735S LCD TEST");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Pin config from schematic:");
    ESP_LOGI(TAG, "  CS=%d  MOSI=%d  CLK=%d  DC=%d  RST=%d",
             PIN_LCD_CS, PIN_LCD_MOSI, PIN_LCD_CLK, PIN_LCD_DC, PIN_LCD_RST);
    ESP_LOGI(TAG, "Screen: ST7735S 128x160 RGB565\n");
    
    // Init
    lcd_spi_init();
    lcd_gpio_init();
    lcd_reset();
    
    // ST7735S Initialization
    ESP_LOGI(TAG, "Initializing ST7735S controller...");
    
    lcd_write_cmd(0x11);           // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(120));
    
    lcd_write_cmd(0xB1);           // Frame Rate Control
    lcd_write_data((uint8_t[]){0x00, 0x1B}, 2);
    
    lcd_write_cmd(0xB3);           // Display Inversion Control
    lcd_write_data((uint8_t[]){0x03}, 1);
    
    lcd_write_cmd(0xB4);           // Display Inversion Control
    lcd_write_data((uint8_t[]){0x07}, 1);
    
    lcd_write_cmd(0xC0);           // Power Control 1
    lcd_write_data((uint8_t[]){0xA2, 0x02, 0x84}, 3);
    
    lcd_write_cmd(0xC1);           // Power Control 2
    lcd_write_data((uint8_t[]){0x0C}, 1);
    
    lcd_write_cmd(0xC5);           // VCOM Control
    lcd_write_data((uint8_t[]){0x0D}, 1);
    
    lcd_write_cmd(0xC7);           // VCOM Offset Control
    lcd_write_data((uint8_t[]){0x0D}, 1);
    
    lcd_write_cmd(0xC8);           // GAMMA
    lcd_write_data((uint8_t[]){0x04, 0x07, 0x07, 0x04,
                               0x04, 0x06, 0x06, 0x00,
                               0x02, 0x05}, 10);
    
    lcd_write_cmd(0x36);           // MX, MY, RGB mode
    lcd_write_data((uint8_t[]){0xC0}, 1);
    
    lcd_write_cmd(0x3A);           // Pixel Format (16-bit RGB565)
    lcd_write_data((uint8_t[]){0x05}, 1);
    
    lcd_write_cmd(0x29);           // Display ON
    
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "✓ LCD Initialized!\n");
    
    // Start display tests
    ESP_LOGI(TAG, "=== DISPLAY TESTS ===\n");
    
    // Solid colors
    show_solid(COLOR_RED, "RED");
    show_solid(COLOR_GREEN, "GREEN");
    show_solid(COLOR_BLUE, "BLUE");
    
    // Color bars
    show_color_bars();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // White and Black
    show_solid(COLOR_WHITE, "WHITE");
    show_solid(COLOR_BLACK, "BLACK");
    
    ESP_LOGI(TAG, "\n=== LOOP STARTED ===\n");
    
    // Flashing loop
    uint16_t flash_colors[] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, 
                               COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA};
    int num_colors = sizeof(flash_colors) / sizeof(flash_colors[0]);
    
    while (1) {
        for (int i = 0; i < num_colors; i++) {
            lcd_fill(flash_colors[i]);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
}
