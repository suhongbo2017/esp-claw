#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new ST7735S SPI LCD panel.
 *
 * Creates the SPI bus, panel IO handler, allocates the panel context,
 * populates the vtable, resets the chip, sends init commands, and returns
 * an opaque esp_lcd_panel_handle_t ready for use with all standard
 * panel operations (reset, init, draw_bitmap, mirror, etc.).
 *
 * @param mosi_gpio    SPI MOSI pin
 * @param sclk_gpio    SPI SCLK pin
 * @param cs_gpio      SPI CS pin
 * @param dc_gpio      Data/Command pin
 * @param rst_gpio     Reset pin (GPIO_NUM_NC if N/C)
 * @param width        Display width in pixels
 * @param height       Display height in pixels
 * @param spi_host     SPI host port (SPI2_HOST or SPI3_HOST)
 * @param clk_hz       SPI clock frequency
 * @param ret_panel    Output: panel handle
 * @return ESP_OK on success
 */
esp_err_t esp_lcd_new_panel_st7735s(
    gpio_num_t mosi_gpio,
    gpio_num_t sclk_gpio,
    gpio_num_t cs_gpio,
    gpio_num_t dc_gpio,
    gpio_num_t rst_gpio,
    int width,
    int height,
    spi_host_device_t spi_host,
    int clk_hz,
    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
