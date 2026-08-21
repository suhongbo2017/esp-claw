/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @brief LCD panel interface definition — local copy for board-level setup_device.c
 *
 * This header replicates esp_lcd_panel_interface.h from ESP-IDF v6.x.
 * In ESP-IDF v5.5, this struct is opaque to application code, so boards
 * that need to embed esp_lcd_panel_t must provide their own definition.
 *
 * Struct layout matches ESP-IDF upstream exactly to ensure vtable compatibility.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Full LCD panel structure definition
 *
 * NOTE: This struct is intentionally defined here for board-level code that
 * needs to populate a vtable. In official ESP-IDF builds, this struct is
 * internal and only accessible from COMPONENT context. Boards embed an
 * instance of this struct as the first member of their context, then cast
 * `esp_lcd_panel_handle_t` back to their context using pointer arithmetic.
 *
 * Layout MUST match ESP-IDF upstream exactly. Adding/removing members or
 * changing order will corrupt the vtable at runtime.
 */
struct esp_lcd_panel_t {
    /**
     * @brief Reset LCD panel
     *
     * @param[in] panel LCD panel handle
     * @return ESP_OK on success
     */
    esp_err_t (*reset)(esp_lcd_panel_t *panel);

    /**
     * @brief Initialize LCD panel after reset
     *
     * @param[in] panel LCD panel handle
     * @return ESP_OK on success
     */
    esp_err_t (*init)(esp_lcd_panel_t *panel);

    /**
     * @brief Destroy and free LCD panel resources
     *
     * @param[in] panel LCD panel handle
     * @return ESP_OK on success
     */
    esp_err_t (*del)(esp_lcd_panel_t *panel);

    /**
     * @brief Draw bitmap on LCD panel
     *
     * @param[in] panel LCD panel handle
     * @param[in] x_start Start pixel index on x-axis (inclusive)
     * @param[in] y_start Start pixel index on y-axis (inclusive)
     * @param[in] x_end End pixel index on x-axis (exclusive)
     * @param[in] y_end End pixel index on y-axis (exclusive)
     * @param[in] color_data RGB color data (pixel format depends on panel)
     * @return ESP_OK on success
     */
    esp_err_t (*draw_bitmap)(esp_lcd_panel_t *panel, int x_start, int y_start,
                              int x_end, int y_end, const void *color_data);

    /**
     * @brief Draw partial bitmap on LCD panel (with source conversion)
     *
     * @param[in] panel LCD panel handle
     * @param[in] x_start Start pixel index on target (inclusive)
     * @param[in] y_start Start pixel index on target (inclusive)
     * @param[in] x_end End pixel index on target (exclusive)
     * @param[in] y_end End pixel index on target (exclusive)
     * @param[in] src_data Source bitmap data
     * @param[in] src_x_size Width of source bitmap in pixels
     * @param[in] src_y_size Height of source bitmap in pixels
     * @param[in] src_x_start Start pixel index on source (inclusive)
     * @param[in] src_y_start Start pixel index on source (inclusive)
     * @param[in] src_x_end End pixel index on source (exclusive)
     * @param[in] src_y_end End pixel index on source (exclusive)
     * @return ESP_OK on success
     */
    esp_err_t (*draw_bitmap_2d)(esp_lcd_panel_t *panel, int x_start, int y_start,
                                 int x_end, int y_end, const void *src_data,
                                 size_t src_x_size, size_t src_y_size,
                                 int src_x_start, int src_y_start, int src_x_end, int src_y_end);

    /**
     * @brief Mirror the LCD panel on specific axes
     *
     * @note Combine with swap_xy for rotation support
     *
     * @param[in] panel LCD panel handle
     * @param[in] x_axis Whether to mirror about x axis
     * @param[in] y_axis Whether to mirror about y axis
     * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not supported
     */
    esp_err_t (*mirror)(esp_lcd_panel_t *panel, bool x_axis, bool y_axis);

    /**
     * @brief Swap X and Y axes
     *
     * @note Combine with mirror for full rotation support
     *
     * @param[in] panel LCD panel handle
     * @param[in] swap_axes Whether to swap X and Y
     * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not supported
     */
    esp_err_t (*swap_xy)(esp_lcd_panel_t *panel, bool swap_axes);

    /**
     * @brief Set extra gap in X and Y axes
     *
     * @note Gap is used for calculating real coordinates
     *
     * @param[in] panel LCD panel handle
     * @param[in] x_gap Extra gap on X axis (pixels)
     * @param[in] y_gap Extra gap on Y axis (pixels)
     * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not supported
     */
    esp_err_t (*set_gap)(esp_lcd_panel_t *panel, int x_gap, int y_gap);

    /**
     * @brief Invert display colors (bitwise complement on color lines)
     *
     * @param[in] panel LCD panel handle
     * @param[in] invert_color_data Whether to enable inversion
     * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not supported
     */
    esp_err_t (*invert_color)(esp_lcd_panel_t *panel, bool invert_color_data);

    /**
     * @brief Turn display on or off
     *
     * @param[in] panel LCD panel handle
     * @param[in] on_off True = on, False = off
     * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not supported
     */
    esp_err_t (*disp_on_off)(esp_lcd_panel_t *panel, bool on_off);

    /**
     * @brief Enter or exit sleep mode
     *
     * @param[in] panel LCD panel handle
     * @param[in] sleep True = enter sleep, False = wake up
     * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not supported
     */
    esp_err_t (*disp_sleep)(esp_lcd_panel_t *panel, bool sleep);

    /**
     * @brief Set display brightness
     *
     * @param[in] panel LCD panel handle
     * @param[in] brightness Brightness value (0–max depends on implementation)
     * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not supported
     */
    esp_err_t (*set_brightness)(esp_lcd_panel_t *panel, int brightness);

    void *user_data;   /**< User data, stored externally for customization */
};

/**
 * @brief Type alias for backward compatibility
 */
typedef struct esp_lcd_panel_t esp_lcd_panel_t;

#ifdef __cplusplus
}
#endif
