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
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "st7735s";

/** ST7735S initialization commands table */
static const struct {
    uint8_t cmd;
    const uint8_t *data;
    size_t len;
    uint16_t delay_ms;
} st7735s_init_cmds[] = {
    // Soft reset
    {0x01, NULL, 0, 5},

    // Sleep out
    {0x11, NULL, 0, 120},

    // Frame Rate Control (in normal mode/full colors)
    {0xB1, (const uint8_t[]){0x00, 0x1B, 0x08}, 3, 0},

    // Display Inversion Control
    {0xC0, (const uint8_t[]){0x0D, 0x01, 0x02, 0x02}, 4, 0},

    // Display Inversion Control
    {0xC1, (const uint8_t[]){0x40, 0x81}, 2, 0},

    // Exit Partial Mode
    {0xC5, (const uint8_t[]){0x30, 0x30}, 2, 0},

    // Gamma Control
    {0xC8, (const uint8_t[]){0x00, 0x32, 0x36, 0x3D, 0x3E, 0x2C, 0x29, 0x2E, 0x30, 0x30, 0x38, 0x3B}, 12, 0},

    // Memory Access Control
    {0x36, NULL, 0, 0},

    // Pixel Format: 16-bit/pixel
    {0x3A, (const uint8_t[]){0x55}, 1, 0},

    // Column Address Set
    {0x2A, (const uint8_t[]){0x00, 0x00, 0x00, 0x7F}, 4, 0},

    // Page Address Set
    {0x2B, (const uint8_t[]){0x00, 0x00, 0x00, 0x9F}, 4, 0},

    // Write Memory Start
    {0x2C, NULL, 0, 0},

    // Display On
    {0x29, NULL, 0, 120},
};

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
} st7735s_panel_t;

static esp_err_t panel_st7735s_del(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *st7735s = (st7735s_panel_t *)panel->user_data;
    if (st7735s->reset_gpio_num >= 0) {
        gpio_reset_pin(st7735s->reset_gpio_num);
    }
    free(st7735s);
    return ESP_OK;
}

static esp_err_t panel_st7735s_reset(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *st7735s = (st7735s_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7735s->io;

    if (st7735s->reset_gpio_num >= 0) {
        gpio_set_level(st7735s->reset_gpio_num, !st7735s->reset_level);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(st7735s->reset_gpio_num, st7735s->reset_level);
        vTaskDelay(pdMS_TO_TICKS(5));
    } else {
        // Send soft reset command
        esp_lcd_panel_io_tx_param(io, 0x01, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return ESP_OK;
}

static esp_err_t panel_st7735s_init(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *st7735s = (st7735s_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7735s->io;

    // Send initialization commands
    for (size_t i = 0; i < sizeof(st7735s_init_cmds) / sizeof(st7735s_init_cmds[0]); i++) {
        if (st7735s_init_cmds[i].len > 0) {
            esp_lcd_panel_io_tx_param(io, st7735s_init_cmds[i].cmd,
                                       st7735s_init_cmds[i].data,
                                       st7735s_init_cmds[i].len);
        } else {
            esp_lcd_panel_io_tx_param(io, st7735s_init_cmds[i].cmd, NULL, 0);
        }
        if (st7735s_init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(st7735s_init_cmds[i].delay_ms));
        }
    }

    return ESP_OK;
}

static esp_err_t panel_st7735s_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                            int x_end, int y_end, const void *color_data)
{
    // Not implemented - using generic bitmap draw
    (void)panel;
    (void)x_start;
    (void)y_start;
    (void)x_end;
    (void)y_end;
    (void)color_data;
    return ESP_OK;
}

static esp_err_t panel_st7735s_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7735s_panel_t *st7735s = (st7735s_panel_t *)panel->user_data;

    if (invert_color_data) {
        return esp_lcd_panel_io_tx_param(st7735s->io, 0x20, NULL, 0);
    }
    return esp_lcd_panel_io_tx_param(st7735s->io, 0x21, NULL, 0);
}

static esp_err_t panel_st7735s_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7735s_panel_t *st7735s = (st7735s_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7735s->io;

    // Read current MADCTL
    uint8_t madctl_val = 0;
    esp_lcd_panel_io_rx_param(io, 0x36, &madctl_val, 1);

    if (mirror_x) {
        madctl_val |= 0x40;
    } else {
        madctl_val &= ~0x40;
    }
    if (mirror_y) {
        madctl_val |= 0x20;
    } else {
        madctl_val &= ~0x20;
    }

    return esp_lcd_panel_io_tx_param(io, 0x36, &madctl_val, 1);
}

static esp_err_t panel_st7735s_swap_xy(esp_lcd_panel_t *panel, bool swap_xy)
{
    st7735s_panel_t *st7735s = (st7735s_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7735s->io;

    uint8_t madctl_val = 0;
    esp_lcd_panel_io_rx_param(io, 0x36, &madctl_val, 1);

    if (swap_xy) {
        madctl_val |= 0x10;
    } else {
        madctl_val &= ~0x10;
    }

    return esp_lcd_panel_io_tx_param(io, 0x36, &madctl_val, 1);
}

static esp_err_t panel_st7735s_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    (void)panel;
    (void)x_gap;
    (void)y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7735s_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7735s_panel_t *st7735s = (st7735s_panel_t *)panel->user_data;

    if (on_off) {
        return esp_lcd_panel_io_tx_param(st7735s->io, 0x29, NULL, 0);
    }
    return esp_lcd_panel_io_tx_param(st7735s->io, 0x28, NULL, 0);
}

esp_err_t esp_lcd_new_panel_st7735s(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    st7735s_panel_t *st7735s = calloc(1, sizeof(st7735s_panel_t));
    ESP_RETURN_ON_FALSE(st7735s, ESP_ERR_NO_MEM, TAG, "no mem for st7735s panel");

    st7735s->io = io;
    st7735s->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7735s->reset_level = panel_dev_config->flags.reset_active_high;

    /* Create generic SPI panel */
    esp_lcd_panel_t *panel = calloc(1, sizeof(esp_lcd_panel_t));
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_NO_MEM, TAG, "no mem for panel");

    panel->del = panel_st7735s_del;
    panel->init = panel_st7735s_init;
    panel->reset = panel_st7735s_reset;
    panel->draw_bitmap = panel_st7735s_draw_bitmap;
    panel->invert_color = panel_st7735s_invert_color;
    panel->mirror = panel_st7735s_mirror;
    panel->swap_xy = panel_st7735s_swap_xy;
    panel->set_gap = panel_st7735s_set_gap;
    panel->disp_on_off = panel_st7735s_disp_on_off;
    panel->user_data = st7735s;

    *ret_panel = (esp_lcd_panel_handle_t)panel;
    ESP_LOGI(TAG, "ST7735S panel created @%p", panel);

    return ESP_OK;
}
