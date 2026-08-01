/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_lcd_st7735s.h"

#include <string.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "st7735s";

/** ST7735S initialization commands */
static const struct {
    uint8_t cmd;
    const uint8_t *data;
    size_t len;
    uint16_t delay_ms;
} st7735s_init_cmds[] = {
    {0x01, NULL, 0, 5},           // Soft reset
    {0x11, NULL, 0, 120},         // Sleep out
    {0x3A, (const uint8_t[]){0x05}, 1, 0},  // Interface pixel format: 16-bit/pixel
    {0x29, NULL, 0, 120},         // Display on

    // Power control
    {0xCF, (const uint8_t[]){0x00, 0xC1, 0x30}, 3, 0},
    {0xE1, (const uint8_t[]){0x00, 0x00}, 2, 0},

    // Gamma control
    {0x2A, (const uint8_t[]){0x00, 0x00, 0x00, 0xFF}, 4, 0},  // Column address
    {0x2B, (const uint8_t[]){0x00, 0x00, 0x00, 0xFF}, 4, 0},  // Row address
    {0x2C, NULL, 0, 0},           // Write memory

    // Display inversion
    {0xB0, (const uint8_t[]){0x00}, 1, 0},  // Partial display mode
    {0xB1, (const uint8_t[]){0x00, 0x18}, 2, 0},  // Frame rate control
    {0xB4, (const uint8_t[]){0x00}, 1, 0},  // Display inversion control
    {0xB6, (const uint8_t[]){0x02, 0x02}, 2, 0},  // Display function control

    // Power control 2
    {0xC0, (const uint8_t[]){0x0A, 0x0A}, 2, 0},  // Power control 1
    {0xC1, (const uint8_t[]){0x02}, 1, 0},  // Power control 2
    {0xC2, (const uint8_t[]){0x01}, 1, 0},  // Power control 3
    {0xC3, (const uint8_t[]){0x0A, 0x0A}, 2, 0},  // Power control 4
    {0xC4, (const uint8_t[]){0x0A}, 1, 0},  // Power control 5
    {0xC5, (const uint8_t[]){0x00}, 1, 0},  // VMCOM control

    // Memory access control
    {0x36, (const uint8_t[]){0x00}, 1, 0},  // Memory access control

    // Display function control
    {0x3C, (const uint8_t[]){0x01}, 1, 0},  // Frame rate control
    {0x51, (const uint8_t[]){0xFF}, 1, 0},  // Display brightness
    {0x53, (const uint8_t[]){0x00}, 1, 0},  // Write display power on
};

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_dev_config_t dev_config;
    esp_lcd_panel_handle_t panel;
    bool initialized;
} st7735s_panel_t;

esp_err_t esp_lcd_new_panel_st7735s(esp_lcd_panel_io_handle_t panel_io,
                                     const esp_lcd_st7735s_config_t *config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(config != NULL && ret_panel != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    st7735s_panel_t *panel = calloc(1, sizeof(st7735s_panel_t));
    ESP_RETURN_ON_FALSE(panel != NULL, ESP_ERR_NO_MEM, TAG, "failed to allocate memory");

    panel->io = panel_io;
    panel->initialized = false;

    // Set default configuration if not provided
    if (config->width == 0) {
        config->width = 128;
    }
    if (config->height == 0) {
        config->height = 160;
    }
    if (config->bpp == 0) {
        config->bpp = 16;
    }

    // Create panel device configuration
    esp_lcd_panel_dev_config_t dev_config = {
        .reset_gpio_num = config->reset_gpio_num,
        .rgb_ele_order = (config->rgb_order == 1) ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = config->bpp,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .flags = {
            .reset_active_high = true,
        },
    };

    // Send initialization commands
    for (size_t i = 0; i < sizeof(st7735s_init_cmds) / sizeof(st7735s_init_cmds[0]); i++) {
        esp_err_t ret = esp_lcd_panel_io_tx_param(panel_io, st7735s_init_cmds[i].cmd,
                                                   st7735s_init_cmds[i].data,
                                                   st7735s_init_cmds[i].len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send init cmd 0x%02x: %s", st7735s_init_cmds[i].cmd, esp_err_to_name(ret));
            free(panel);
            return ret;
        }
        if (st7735s_init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(st7735s_init_cmds[i].delay_ms));
        }
    }

    // Reset and initialize panel
    if (config->reset_gpio_num >= 0) {
        esp_lcd_panel_reset(panel_io);
    }
    esp_lcd_panel_init(panel_io);

    panel->initialized = true;
    *ret_panel = (esp_lcd_panel_handle_t)panel;

    ESP_LOGI(TAG, "ST7735S panel created: %dx%d", config->width, config->height);
    return ESP_OK;
}

esp_err_t esp_lcd_delete_panel_st7735s(esp_lcd_panel_handle_t panel_handle)
{
    if (!panel_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    st7735s_panel_t *panel = (st7735s_panel_t *)panel_handle;

    if (panel->initialized) {
        // Turn off display
        esp_lcd_panel_io_tx_cmd(panel->io, 0x28, NULL, 0);  // Display off
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    free(panel);
    return ESP_OK;
}
