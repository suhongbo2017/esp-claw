/**
 * ESP-CLAW LCD Display Component
 * Driver for 1.77" ST7735S TFT (128x160) via GPIO bit-bang SPI.
 *
 * Hardware pins:
 *   MOSI  = GPIO17
 *   SCLK  = GPIO18
 *   CS    = GPIO21
 *   RST   = GPIO15
 *   DC    = GPIO16
 *
 * Architecture decision: Pure GPIO bit-bang instead of ESP-IDF SPI DMA,
 * because ESP32-S3 SPI DMA transfers get interrupted by subsequent operations
 * causing silent data loss in pixel writes.
 */

#ifndef ESPCLAW_LCD_DISPLAY_H
#define ESPCLAW_LCD_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the LCD display hardware.
 * Calls lcd_init_sequence() internally and waits for display stabilization.
 * @return 0 on success, non-zero error code otherwise.
 */
esp_err_t lcd_display_init(void);

/**
 * Fill entire screen with a solid color (RGB565).
 * Uses continuous CS policy to prevent ST7735S from interpreting mid-stream
 * as frame boundary — this is the key fix for the "flash then blank" bug.
 * @param color RGB565 color value.
 */
void lcd_fill_screen(uint16_t color);

/**
 * Turn off the LCD backlight / disable display output.
 */
void lcd_display_off(void);

/**
 * Get current fill speed stats (for debug/reporting).
 */
int lcd_get_fill_speed_kbps(void);

/**
 * Test pattern: cycle through all standard colors (WHITE, BLACK, RED, GREEN, BLUE).
 * Each color displayed for ~500ms. Useful for hardware verification.
 */
void lcd_run_color_test(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPCLAW_LCD_DISPLAY_H */
