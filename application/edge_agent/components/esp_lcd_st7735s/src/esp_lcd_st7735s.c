#include "esp_lcd_st7735s.h"

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Internal header with FULL esp_lcd_panel_t struct definition.
 * Available in COMPONENT build context only (not APPLICATION). */
#include "esp_lcd_panel_interface.h"

static const char *TAG = "st7735s_panel";

/* Helper: convert panel handle back to our context struct.
 * Safe because base is always the first member. */
#define PANEL_TO_CTX(panel) \
    ((st7735s_panel_t *)((uint8_t *)(panel) - offsetof(st7735s_panel_t, base)))

/* Init-command table — from ST7735S datasheet */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    uint32_t delay_ms;
} init_cmd_t;

static const init_cmd_t s_init_cmds[] = {
    {0x01, {}, 0, 20},                            /* Software Reset */
    {0x11, {}, 0, 120},                           /* Sleep Out */
    {0xB1, {0x00, 0x1B, 0x08}, 3, 0},            /* Frame Rate Ctrl */
    {0xC0, {0x0D, 0x01, 0x02, 0x02}, 4, 0},      /* Display Inversion Ctrl */
    {0xC1, {0x40, 0x81}, 2, 0},                  /* Display Inversion Ctrl (2) */
    {0xC5, {0x30, 0x30}, 2, 0},                  /* VCOM Control */
    {0xC8, {0x00,0x32,0x36,0x3D,0x3E,0x2C,0x29,0x2E,0x30,0x30,0x38,0x3B}, 12, 0}, /* Gamma */
    {0x3A, {0x55}, 1, 0},                        /* Pixel Format: 16-bit RGB565 */
    {0x2A, {0x00, 0x00, 0x00, 0x7F}, 4, 0},     /* Column Address Set */
    {0x2B, {0x00, 0x00, 0x00, 0x9F}, 4, 0},     /* Page Address Set */
    {0x29, {}, 0, 120},                          /* Display On */
};

/* Send init commands with MADCTL injection at 0x36 */
static esp_err_t send_init(esp_lcd_panel_io_handle_t io, uint8_t madctl)
{
    for (size_t i = 0; i < sizeof(s_init_cmds)/sizeof(s_init_cmds[0]); i++) {
        const init_cmd_t *c = &s_init_cmds[i];
        const uint8_t *buf = c->data;
        size_t len = c->len;

        uint8_t local_buf[1] = { madctl };
        if (c->cmd == 0x36 && c->len == 0) {
            buf = local_buf;
            len = 1;
        }

        esp_err_t ret = esp_lcd_panel_io_tx_param(io, c->cmd, buf, len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "init cmd 0x%02X failed", c->cmd);
            return ret;
        }
        if (c->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    return ESP_OK;
}

/* ---- Panel context ---- */
typedef struct {
    esp_lcd_panel_t base;           /* VTABLE — MUST BE FIRST per IDF convention */
    esp_lcd_panel_io_handle_t io;
    gpio_num_t rst_gpio;
    uint8_t madctl;
} st7735s_panel_t;

/* ---- Callbacks ---- */

static esp_err_t cb_del(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    if (p->rst_gpio >= 0) {
        gpio_reset_pin(p->rst_gpio);
    }
    free(p);
    return ESP_OK;
}

static esp_err_t cb_reset(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    if (p->rst_gpio >= 0) {
        gpio_set_level(p->rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(p->rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        esp_lcd_panel_io_tx_param(p->io, 0x01, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t cb_init(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    ESP_LOGI(TAG, "init seq (MADCTL=0x%02X)", p->madctl);
    return send_init(p->io, p->madctl);
}

static esp_err_t cb_draw_bitmap(esp_lcd_panel_t *panel, int xs, int ys,
                                 int xe, int ye, const void *color_data)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    uint8_t caset[4] = {(uint8_t)(xs>>8),(uint8_t)xs,(uint8_t)((xe-1)>>8),(uint8_t)(xe-1)};
    uint8_t raset[4] = {(uint8_t)(ys>>8),(uint8_t)ys,(uint8_t)((ye-1)>>8),(uint8_t)(ye-1)};
    esp_lcd_panel_io_tx_param(p->io, 0x2A, caset, 4);
    esp_lcd_panel_io_tx_param(p->io, 0x2B, raset, 4);
    esp_lcd_panel_io_tx_param(p->io, 0x2C, NULL, 0);
    size_t len = (size_t)(xe - xs) * (ye - ys) * 2;
    esp_lcd_panel_io_tx_param(p->io, 0x2C, color_data, len);
    return ESP_OK;
}

static esp_err_t cb_mirror(esp_lcd_panel_t *panel, bool mx, bool my)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    uint8_t v = p->madctl;
    if (mx) v |= 0x40; else v &= ~0x40;
    if (my) v |= 0x20; else v &= ~0x20;
    return esp_lcd_panel_io_tx_param(p->io, 0x36, &v, 1);
}

static esp_err_t cb_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    uint8_t v = p->madctl;
    if (swap) v |= 0x10; else v &= ~0x10;
    return esp_lcd_panel_io_tx_param(p->io, 0x36, &v, 1);
}

static esp_err_t cb_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    uint8_t cmd = invert ? 0x21 : 0x20;
    return esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
}

static esp_err_t cb_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    uint8_t cmd = on ? 0x29 : 0x28;
    return esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
}

static esp_err_t cb_set_gap(esp_lcd_panel_t *panel, int x, int y)
{
    (void)panel; (void)x; (void)y;
    return ESP_OK;
}

static esp_err_t cb_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7735s_panel_t *p = PANEL_TO_CTX(panel);
    uint8_t cmd = sleep ? 0x10 : 0x11;
    uint32_t delay = sleep ? 120 : 0;
    esp_err_t ret = esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
    if (!sleep && ret == ESP_OK) vTaskDelay(pdMS_TO_TICKS(delay));
    return ret;
}

/* ---- Public factory function ---- */
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
    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(ret_panel != NULL, ESP_ERR_INVALID_ARG, TAG, "null output");

    esp_err_t ret = ESP_OK;
    st7735s_panel_t *p = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    uint8_t madctl = 0x20;  /* portrait orientation (MY=1) */

    /* 1. GPIO config */
    if (rst_gpio != GPIO_NUM_NC) {
        gpio_config_t cfg = {
            .pin_bit_mask = BIT64(rst_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&cfg), err_out, TAG, "GPIO config failed");
    }

    /* 2. SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = mosi_gpio,
        .miso_io_num   = -1,
        .sclk_io_num   = sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = width * 10,
    };
    ESP_GOTO_ON_ERROR(spi_bus_initialize(spi_host, &bus_cfg, SPI_DMA_CH_AUTO),
                      err_spi, TAG, "SPI bus init failed");

    /* 3. Panel IO */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num  = dc_gpio,
        .cs_gpio_num  = cs_gpio,
        .pclk_hz      = clk_hz,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode     = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host,
                                                &io_cfg, &io_handle),
                      err_spi, TAG, "Panel IO init failed");

    /* 4. Allocate panel */
    p = calloc(1, sizeof(*p));
    ESP_GOTO_ON_FALSE(p != NULL, ESP_ERR_NO_MEM, err_io, TAG, "alloc failed");

    p->io         = io_handle;
    p->rst_gpio   = rst_gpio;
    p->madctl     = madctl;

    /* 5. Populate vtable */
    p->base.reset       = cb_reset;
    p->base.init        = cb_init;
    p->base.del         = cb_del;
    p->base.draw_bitmap = cb_draw_bitmap;
    p->base.mirror      = cb_mirror;
    p->base.swap_xy     = cb_swap_xy;
    p->base.set_gap     = cb_set_gap;
    p->base.invert_color = cb_invert_color;
    p->base.disp_on_off = cb_disp_on_off;
    p->base.disp_sleep  = cb_disp_sleep;

    /* 6. Reset + init */
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(&p->base), err_panel, TAG, "panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(&p->base), err_panel, TAG, "panel init failed");

    *ret_panel = &p->base;
    ESP_LOGI(TAG, "ST7735S ready (%d×%d @ %d MHz)", width, height, clk_hz / 1000000);
    return ESP_OK;

err_panel:
    free(p);
err_io:
    esp_lcd_panel_io_del(io_handle);
err_spi:
    spi_bus_free(spi_host);
    if (rst_gpio != GPIO_NUM_NC) {
        gpio_reset_pin(rst_gpio);
    }
err_out:
    return ret;
}
