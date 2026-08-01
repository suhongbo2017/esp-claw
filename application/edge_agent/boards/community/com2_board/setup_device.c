#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_board_manager_includes.h"
#include "gen_board_device_custom.h"

/* Full esp_lcd_panel_t struct definition for embedding.
 * Matched against esp_lcd_panel_interface.h from ESP-IDF v5.5. */
struct esp_lcd_panel_t {
    esp_err_t (*reset)(struct esp_lcd_panel_t *panel);
    esp_err_t (*init)(struct esp_lcd_panel_t *panel);
    esp_err_t (*del)(struct esp_lcd_panel_t *panel);
    esp_err_t (*draw_bitmap)(struct esp_lcd_panel_t *panel, int x_start, int y_start,
                              int x_end, int y_end, const void *color_data);
    esp_err_t (*mirror)(struct esp_lcd_panel_t *panel, bool x_axis, bool y_axis);
    esp_err_t (*swap_xy)(struct esp_lcd_panel_t *panel, bool swap_axes);
    esp_err_t (*set_gap)(struct esp_lcd_panel_t *panel, int x_gap, int y_gap);
    esp_err_t (*invert_color)(struct esp_lcd_panel_t *panel, bool invert_color_data);
    esp_err_t (*disp_on_off)(struct esp_lcd_panel_t *panel, bool on_off);
    esp_err_t (*disp_sleep)(struct esp_lcd_panel_t *panel, bool sleep);
    esp_err_t (*set_brightness)(struct esp_lcd_panel_t *panel, int brightness);
    void *user_data;
};

typedef struct esp_lcd_panel_t st7735s_panel_t;

static const char *TAG = "COM2_BOARD_SETUP_DEVICE";

#define USB_UVC_DEV_NUM             1
#define USB_HOST_TASK_PRIORITY      5
#define USB_HOST_TASK_STACK_SIZE    4096
#define USB_UVC_TASK_PRIORITY       configMAX_PRIORITIES - 2
#define USB_UVC_TASK_STACK_SIZE     4096

typedef struct {
    dev_camera_handle_t handle;
} custom_usb_camera_handle_t;

/* ================================================================
 * USB UVC Camera
 * ================================================================ */

static int usb_camera_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid camera handle");

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    const esp_video_init_usb_uvc_config_t usb_uvc_config = {
        .uvc = {
            .uvc_dev_num = USB_UVC_DEV_NUM,
            .task_stack = USB_UVC_TASK_STACK_SIZE,
            .task_priority = USB_UVC_TASK_PRIORITY,
            .task_affinity = 0,
        },
        .usb = {
            .init_usb_host_lib = true,
            .task_stack = USB_HOST_TASK_STACK_SIZE,
            .task_priority = USB_HOST_TASK_PRIORITY,
            .task_affinity = 0,
        },
    };
    const esp_video_init_config_t video_config = {
        .usb_uvc = &usb_uvc_config,
    };

    esp_err_t ret = esp_video_init(&video_config);
    if (ret != ESP_OK) {
        return ret;
    }

    custom_usb_camera_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        (void)esp_video_deinit();
        return ESP_ERR_NO_MEM;
    }

    handle->handle.dev_path = ESP_VIDEO_USB_UVC_NAME(0);
    handle->handle.meta_path = "";
    *device_handle = &handle->handle;
    ESP_LOGI(TAG, "USB UVC camera initialized, dev_path: %s", handle->handle.dev_path);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "USB UVC camera disabled. Enable CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static int usb_camera_deinit(void *device_handle)
{
#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    free(device_handle);
    ESP_RETURN_ON_ERROR(esp_video_deinit(), TAG, "failed to deinit USB UVC camera");
    ESP_LOGI(TAG, "USB UVC camera deinitialized");
    return ESP_OK;
#else
    (void)device_handle;
    return ESP_OK;
#endif
}

CUSTOM_DEVICE_IMPLEMENT(camera, usb_camera_init, usb_camera_deinit);

/* ================================================================
 * ST7735S LCD — Fully Inlined Driver
 *
 * No external header dependencies. All internals defined inline.
 * Matches proven pattern from waveshare_rlcd_4_2.
 *
 * GPIO Pinout:
 *   MOSI = GPIO17, SCLK = GPIO18, CS = GPIO21, DC = GPIO16, RST = GPIO15
 * ================================================================ */

static const char *TAG_LCD = "ST7735S";

/* ── Pin assignments ─────────────────────────────────────────────────────── */
#define LCD_MOSI_GPIO   GPIO_NUM_17
#define LCD_SCK_GPIO    GPIO_NUM_18
#define LCD_CS_GPIO     GPIO_NUM_21
#define LCD_DC_GPIO     GPIO_NUM_16
#define LCD_RST_GPIO    GPIO_NUM_15

/* ── Display geometry ────────────────────────────────────────────────────── */
#define LCD_WIDTH       128
#define LCD_HEIGHT      160

/* ── SPI configuration ───────────────────────────────────────────────────── */
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_SPI_CLK_HZ  (40 * 1000 * 1000)  /* 40 MHz */

/* ── Context structure ───────────────────────────────────────────────────── */
typedef struct {
    esp_lcd_panel_t base;                   /* VTABLE embedded — MUST BE FIRST */
    esp_lcd_panel_io_handle_t io;           /* Panel IO handle */
    gpio_num_t rst_gpio;                    /* Reset pin */
    uint8_t madctl;                         /* MADCTL byte for orientation */
} lcd_context_t;

/* ── Init-command table — from ST7735S datasheet ─────────────────────────── */
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

/* Send init commands with MADCTL injection at command 0x36 */
static esp_err_t send_init_cmds(esp_lcd_panel_io_handle_t io, uint8_t madctl)
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
            ESP_LOGE(TAG_LCD, "Init cmd 0x%02X failed: %s", c->cmd, esp_err_to_name(ret));
            return ret;
        }
        if (c->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    return ESP_OK;
}

/* ── VTABLE callback implementations ─────────────────────────────────────── */

static esp_err_t cb_del(st7735s_panel_t *panel)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    if (ctx->rst_gpio >= 0) {
        gpio_reset_pin(ctx->rst_gpio);
    }
    free(panel);
    return ESP_OK;
}

static esp_err_t cb_reset(st7735s_panel_t *panel)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    if (ctx->rst_gpio >= 0) {
        gpio_set_level(ctx->rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(ctx->rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        esp_lcd_panel_io_tx_param(ctx->io, 0x01, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t cb_init(st7735s_panel_t *panel)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    ESP_LOGI(TAG_LCD, "Sending init seq (MADCTL=0x%02X)", ctx->madctl);
    return send_init_cmds(ctx->io, ctx->madctl);
}

static esp_err_t cb_draw_bitmap(st7735s_panel_t *panel, int xs, int ys,
                                 int xe, int ye, const void *color_data)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    uint8_t caset[4] = {(uint8_t)(xs>>8),(uint8_t)xs,(uint8_t)((xe-1)>>8),(uint8_t)(xe-1)};
    uint8_t raset[4] = {(uint8_t)(ys>>8),(uint8_t)ys,(uint8_t)((ye-1)>>8),(uint8_t)(ye-1)};
    esp_lcd_panel_io_tx_param(ctx->io, 0x2A, caset, 4);
    esp_lcd_panel_io_tx_param(ctx->io, 0x2B, raset, 4);
    esp_lcd_panel_io_tx_param(ctx->io, 0x2C, NULL, 0);
    size_t len = (size_t)(xe - xs) * (ye - ys) * 2;
    esp_lcd_panel_io_tx_param(ctx->io, 0x2C, color_data, len);
    return ESP_OK;
}

static esp_err_t cb_mirror(st7735s_panel_t *panel, bool mx, bool my)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    uint8_t v = ctx->madctl;
    if (mx) v |= 0x40; else v &= ~0x40;
    if (my) v |= 0x20; else v &= ~0x20;
    return esp_lcd_panel_io_tx_param(ctx->io, 0x36, &v, 1);
}

static esp_err_t cb_swap_xy(st7735s_panel_t *panel, bool swap)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    uint8_t v = ctx->madctl;
    if (swap) v |= 0x10; else v &= ~0x10;
    return esp_lcd_panel_io_tx_param(ctx->io, 0x36, &v, 1);
}

static esp_err_t cb_invert_color(st7735s_panel_t *panel, bool invert)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    uint8_t cmd = invert ? 0x21 : 0x20;
    return esp_lcd_panel_io_tx_param(ctx->io, cmd, NULL, 0);
}

static esp_err_t cb_disp_on_off(st7735s_panel_t *panel, bool on)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    uint8_t cmd = on ? 0x29 : 0x28;
    return esp_lcd_panel_io_tx_param(ctx->io, cmd, NULL, 0);
}

static esp_err_t cb_set_gap(st7735s_panel_t *panel, int x, int y)
{
    (void)panel; (void)x; (void)y;
    return ESP_OK;
}

static esp_err_t cb_disp_sleep(st7735s_panel_t *panel, bool sleep)
{
    lcd_context_t *ctx = (lcd_context_t *)panel;
    uint8_t cmd = sleep ? 0x10 : 0x11;
    uint32_t delay = sleep ? 120 : 0;
    esp_err_t ret = esp_lcd_panel_io_tx_param(ctx->io, cmd, NULL, 0);
    if (!sleep && ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
    return ret;
}

/* ── Report configuration back to board-manager ─────────────────────────── */
static const dev_display_lcd_config_t s_lcd_cfg = {
    .name         = "display_lcd",
    .chip         = "st7735",
    .sub_type     = "spi",
    .lcd_width    = LCD_WIDTH,
    .lcd_height   = LCD_HEIGHT,
    .swap_xy      = true,
    .mirror_x     = false,
    .mirror_y     = true,
    .invert_color = false,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .data_endian  = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
};

static dev_display_lcd_handles_t s_lcd_handles;

/* ── display_lcd custom device lifecycle ─────────────────────────────────── */
static int display_lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config; (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG,
                        TAG_LCD, "null handle");

    esp_err_t ret = ESP_OK;
    lcd_context_t *p = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    uint8_t madctl = 0x20;  /* portrait orientation (MY=1) */

    /* ── 1. Configure RST GPIO ──────────────────────────────────────── */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(LCD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type  = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&rst_cfg), err_out, TAG_LCD, "RST GPIO config failed");

    /* ── 2. Initialise SPI bus ──────────────────────────────────────── */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = LCD_MOSI_GPIO,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 10,  /* 10 lines of RGB565 */
    };
    ESP_GOTO_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                      err_spi, TAG_LCD, "SPI bus init failed");

    /* ── 3. Create panel IO (SPI) ───────────────────────────────────── */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num  = LCD_DC_GPIO,
        .cs_gpio_num  = LCD_CS_GPIO,
        .pclk_hz      = LCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode     = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                &io_cfg, &io_handle),
                      err_spi, TAG_LCD, "Panel IO init failed");

    /* ── 4. Allocate panel context (with embedded esp_lcd_panel_t base) ─ */
    p = calloc(1, sizeof(lcd_context_t));
    ESP_GOTO_ON_FALSE(p != NULL, ESP_ERR_NO_MEM, err_io, TAG_LCD, "alloc failed");

    p->io       = io_handle;
    p->rst_gpio = LCD_RST_GPIO;
    p->madctl   = madctl;

    /* ── 5. Populate vtable ─────────────────────────────────────────── */
    p->base.reset       = (esp_err_t (*)(struct esp_lcd_panel_t *))cb_reset;
    p->base.init        = (esp_err_t (*)(struct esp_lcd_panel_t *))cb_init;
    p->base.del         = (esp_err_t (*)(struct esp_lcd_panel_t *))cb_del;
    p->base.draw_bitmap = (esp_err_t (*)(struct esp_lcd_panel_t *, int, int, int, int, const void *))cb_draw_bitmap;
    p->base.mirror      = (esp_err_t (*)(struct esp_lcd_panel_t *, bool, bool))cb_mirror;
    p->base.swap_xy     = (esp_err_t (*)(struct esp_lcd_panel_t *, bool))cb_swap_xy;
    p->base.set_gap     = (esp_err_t (*)(struct esp_lcd_panel_t *, int, int))cb_set_gap;
    p->base.invert_color = (esp_err_t (*)(struct esp_lcd_panel_t *, bool))cb_invert_color;
    p->base.disp_on_off = (esp_err_t (*)(struct esp_lcd_panel_t *, bool))cb_disp_on_off;
    p->base.disp_sleep  = (esp_err_t (*)(struct esp_lcd_panel_t *, bool))cb_disp_sleep;

    /* ── 6. Reset & initialize panel ────────────────────────────────── */
    esp_lcd_panel_reset(&p->base);
    ret = esp_lcd_panel_init(&p->base);
    if (ret != ESP_OK) {
        ESP_GOTO_ON_FALSE(false, ret, err_free_panel, TAG_LCD, "panel init failed");
    }

    /* ── 7. Register with board manager ─────────────────────────────── */
    s_lcd_handles.panel_handle = &p->base;
    esp_board_device_override_config("display_lcd", &s_lcd_cfg, sizeof(s_lcd_cfg));
    *device_handle = &s_lcd_handles;

    ESP_LOGI(TAG_LCD, "ST7735S ready (%d×%d @ %d MHz)", LCD_WIDTH, LCD_HEIGHT,
             LCD_SPI_CLK_HZ / 1000000);
    return ESP_OK;

err_free_panel:
    free(p);
err_free_io:
    esp_lcd_panel_io_del(io_handle);
err_spi:
    spi_bus_free(LCD_SPI_HOST);
    gpio_reset_pin(LCD_RST_GPIO);
err_out:
    return ret;
}

static int display_lcd_deinit(void *device_handle)
{
    dev_display_lcd_handles_t *h = (dev_display_lcd_handles_t *)device_handle;
    if (h != NULL) {
        if (h->panel_handle != NULL) {
            esp_lcd_panel_del(h->panel_handle);
            h->panel_handle = NULL;
        }
    }
    spi_bus_free(LCD_SPI_HOST);
    ESP_LOGI(TAG_LCD, "ST7735S deinitialised");
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);
