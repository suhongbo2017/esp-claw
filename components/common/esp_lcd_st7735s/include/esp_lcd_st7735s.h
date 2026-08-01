/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ST7735S panel device configuration
 */
typedef struct {
    int reset_gpio_num;           /**< GPIO number for RESET pin (-1 if not used) */
    int dc_gpio_num;              /**< GPIO number for DC/RS pin */
    int cs_gpio_num;              /**< GPIO number for CS pin */
    int backlight_gpio_num;       /**< GPIO number for backlight (-1 if not used) */
    int width;                    /**< Display width in pixels */
    int height;                   /**< Display height in pixels */
    uint8_t mx;                   /**< Horizontal mirror */
    uint8_t my;                   /**< Vertical mirror */
    uint8_t mx_byte_order;        /**< Horizontal byte order */
    uint8_t my_byte_order;        /**< Vertical byte order */
    uint8_t rgb_order;            /**< Color order (0: RGB, 1: BGR) */
    uint8_t if_format;            /**< Interface format */
    uint8_t bpp;                  /**< Bits per pixel */
    uint8_t flip;                 /**< Flip vertical */
    uint8_t vsp翻转;              /**< Vertical scroll */
    uint8_t x_start;              /**< Start X position */
    uint8_t y_start;              /**< Start Y position */
} esp_lcd_st7735s_config_t;

/**
 * @brief Create a new ST7735S panel device
 *
 * @param panel_io Panel IO handle
 * @param config Panel configuration
 * @param ret_panel Output panel handle
 * @return ESP_OK on success, error code on failure
 */
esp_err_t esp_lcd_new_panel_st7735s(esp_lcd_panel_io_handle_t panel_io,
                                     const esp_lcd_st7735s_config_t *config,
                                     esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief Delete ST7735S panel device
 *
 * @param panel Panel handle
 * @return ESP_OK on success, error code on failure
 */
esp_err_t esp_lcd_delete_panel_st7735s(esp_lcd_panel_handle_t panel);

#ifdef __cplusplus
}
#endif
