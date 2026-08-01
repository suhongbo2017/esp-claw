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

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#endif

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
 * USB UVC Camera — unchanged from original com2_board setup
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
    ESP_LOGE(TAG, "USB UVC camera is disabled. Enable CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE");
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
 * ST7735S LCD Driver for com2_board
 *
 * Implementation pattern based on waveshare_rlcd_4_2:
 *   Define our own struct with esp_lcd_panel_t base as FIRST member.
 *   Populate vtable fields on &s->base, then cast &s->base to handle.
 *
 * Key insight from ESP-IDF v5.5: although esp_lcd_panel_t is declared
 * opaque in headers, the type IS fully defined in the framework source,
 * and we can embed it in our own structs when targeting this specific
 * IDF version. The embedded struct must have esp_lcd_panel_t as the
 * very first member so that casting works correctly.
 * ================================================================ */
static const char *TAG_ST7735S = "ST7735S";

/* ---------- Custom panel object ──────────────────────────────────────── */
typedef struct {
    esp_lcd_panel_t base;              /* VTABLE — MUST BE FIRST */
    esp_lcd_panel_io_handle_t io;
    gpio_num_t rst_gpio;
    bool     reset_active_high;        /* Reset pin polarity */
    uint8_t  madctl;                   /* Memory Access Control byte */
} st7735s_panel_t;

/* ---------- Init command sequence (from ST7735S datasheet + SPI LCD example) --- */
static const struct {
    uint8_t cmd;
    const uint8_t *data;
    size_t len;
    uint16_t delay_ms;
} st7735s_init_cmds[] = {
    {0x01, NULL, 0, 20},                /* Software Reset */
    {0x11, NULL, 0, 120},               /* Sleep Out */
    {0xB1, (uint8_t[]){0x00, 0x1B, 0x08}, 3, 0},    /* Frame Rate Ctrl */
    {0xC0, (uint8_t[]){0x0D, 0x01, 0x02, 0x02}, 4, 0},   /* Display Inversion Ctrl */
    {0xC1, (uint8_t[]){0x40, 0x81}, 2, 0},           /* Display Inversion Ctrl (2) */
    {0xC5, (uint8_t[]){0x30, 0x30}, 2, 0},           /* VCOM Control */
    {0xC8, (uint8_t[]){0x00,0x32,0x36,0x3D,0x3E,0x2C,0x29,0x2E,0x30,0x30,0x38,0x3B}, 12, 0}, /* Gamma */
    {0x36, NULL, 0, 0},                          /* MADCTL — set dynamically */
    {0x3A, (uint8_t[]){0x55}, 1, 0},             /* Pixel Format: 16-bit RGB565 */
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0x7F}, 4, 0},    /* Column Address Set */
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0x9F}, 4, 0},    /* Page Address Set */
    {0x29, NULL, 0, 120},                        /* Display On */
};

/* Send all init commands to ST7735S over SPI IO */
static esp_err_t st7735s_send_init(esp_lcd_panel_io_handle_t io, uint8_t madctl)
{
    for (size_t i = 0; i < sizeof(st7735s_init_cmds)/sizeof(st7735s_init_cmds[0]); i++) {
        const struct { uint8_t cmd; const uint8_t *data; size_t len; uint16_t delay_ms; } *c = &st7735s_init_cmds[i];

        /* For MADCTL command, inject pre-computed value */
        uint8_t local_buf = madctl;
        const uint8_t *data = c->data;
        size_t len = c->len;
        if (c->cmd == 0x36 && !c->data) {
            data = &local_buf;
            len = 1;
        }

        esp_err_t ret = esp_lcd_panel_io_tx_param(io, c->cmd, data, len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_ST7735S, "Init cmd 0x%02X failed: %s", c->cmd, esp_err_to_name(ret));
            return ret;
        }
        if (c->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    return ESP_OK;
}

/* ========== Callback implementations (invoked via panel->vtable) ========== */

static esp_err_t cb_del(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *s = (st7735s_panel_t *)panel;
    if (s->rst_gpio >= 0) {
        gpio_reset_pin(s->rst_gpio);
    }
    free(s);
    return ESP_OK;
}

static esp_err_t cb_reset(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *s = (st7735s_panel_t *)panel;
    if (s->rst_gpio >= 0) {
        gpio_set_level(s->rst_gpio, !s->reset_active_high);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(s->rst_gpio, s->reset_active_high);
        vTaskDelay(pdMS_TO_TICKS(5));
    } else {
        esp_lcd_panel_io_tx_param(s->io, 0x01, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

static esp_err_t cb_init(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *s = (st7735s_panel_t *)panel;
    ESP_LOGI(TAG_ST7735S, "Sending init commands (MADCTL=0x%02X)", s->madctl);
    return st7735s_send_init(s->io, s->madctl);
}

static esp_err_t cb_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                 int x_end, int y_end, const void *color_data)
{
    st7735s_panel_t *s = (st7735s_panel_t *)panel;

    /* Update column address window */
    uint8_t caset[4] = {
        (uint8_t)(x_start >> 8), (uint8_t)x_start,
        (uint8_t)((x_end - 1) >> 8), (uint8_t)(x_end - 1)
    };
    /* Update page address window */
    uint8_t raset[4] = {
        (uint8_t)(y_start >> 8), (uint8_t)y_start,
        (uint8_t)((y_end - 1) >> 8), (uint8_t)(y_end - 1)
    };

    esp_lcd_panel_io_tx_param(s->io, 0x2A, caset, 4);  /* CASET */
    esp_lcd_panel_io_tx_param(s->io, 0x2B, raset, 4);  /* RASET */
    esp_lcd_panel_io_tx_param(s->io, 0x2C, NULL, 0);   /* RAMWR header */

    /* Push pixel color data (RGB565) */
    size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * 2;
    esp_lcd_panel_io_tx_param(s->io, 0x2C, color_data, len);
    return ESP_OK;
}

static esp_err_t cb_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    st7735s_panel_t *s = (st7735s_panel_t *)panel;
    uint8_t cmd = invert ? 0x21 : 0x20;
    return esp_lcd_panel_io_tx_param(s->io, cmd, NULL, 0);
}

static esp_err_t cb_mirror(esp_lcd_panel_t *panel, bool mx, bool my)
{
    st7735s_panel_t *s = (st7735s_panel_t *)panel;
    uint8_t v = s->madctl;
    if (mx) v |= 0x40; else v &= ~0x40;   /* MX bit */
    if (my) v |= 0x20; else v &= ~0x20;   /* MY bit */
    return esp_lcd_panel_io_tx_param(s->io, 0x36, &v, 1);
}

static esp_err_t cb_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    st7735s_panel_t *s = (st7735s_panel_t *)panel;
    uint8_t v = s->madctl;
    if (swap) v |= 0x10; else v &= ~0x10;  /* ML bit */
    return esp_lcd_panel_io_tx_param(s->io, 0x36, &v, 1);
}

static esp_err_t cb_set_gap(esp_lcd_panel_t *panel, int x, int y)
{
    (void)panel; (void)x; (void)y;
    return ESP_OK;  /* ST7735S doesn't support gap offset natively */
}

static esp_err_t cb_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    st7735s_panel_t *s = (st7735s_panel_t *)panel;
    uint8_t cmd = on ? 0x29 : 0x28;
    return esp_lcd_panel_io_tx_param(s->io, cmd, NULL, 0);
}

/* ================================================================
 * Board Manager factory entry point
 * Called by esp_board_manager when creating display_lcd device.
 * Receives the already-created SPI panel IO handle and configuration.
 * Returns a populated esp_lcd_panel_handle_t (opaque pointer).
 * ================================================================ */
esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG_ST7735S, "ST7735S factory: create %dx%d",
             panel_dev_config->width, panel_dev_config->height);

    /* Compute MADCTL from panel_dev_config flags */
    uint8_t madctl = 0;
    /* MX: 1 = column address order reversed */
    if (panel_dev_config->flags.mirror_x) {
        madctl |= 0x40;
    } else {
        madctl &= ~0x40;
    }
    /* MY: 1 = row address order reversed */
    if (panel_dev_config->flags.mirror_y) {
        madctl |= 0x20;
    } else {
        madctl &= ~0x20;
    }
    /* BGR: 1 = RGB pixels have BGR order */
    if (panel_dev_config->flags.pixel_format_bgr) {
        madctl |= 0x04;
    } else {
        madctl &= ~0x04;
    }

    /* Allocate custom panel object with embedded base struct */
    st7735s_panel_t *s = calloc(1, sizeof(*s));
    ESP_RETURN_ON_FALSE(s != NULL, ESP_ERR_NO_MEM, TAG_ST7735S, "alloc failed");

    /* Store references */
    s->io               = io;
    s->rst_gpio         = (gpio_num_t)panel_dev_config->reset_gpio_num;
    s->reset_active_high = panel_dev_config->flags.reset_active_high;
    s->madctl           = madctl;

    /* Populate the vtable inside embedded base struct */
    s->base.del         = cb_del;
    s->base.reset       = cb_reset;
    s->base.init        = cb_init;
    s->base.draw_bitmap = cb_draw_bitmap;
    s->base.invert_color = cb_invert_color;
    s->base.mirror      = cb_mirror;
    s->base.swap_xy     = cb_swap_xy;
    s->base.set_gap     = cb_set_gap;
    s->base.disp_on_off = cb_disp_on_off;

    /* Return the embedded base pointer */
    *ret_panel = (esp_lcd_panel_handle_t)&s->base;
    ESP_LOGI(TAG_ST7735S, "ST7735S panel ready @%p (&s->base)", *ret_panel);

    /* Initialize display immediately */
    esp_err_t ret = esp_lcd_panel_init(*ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ST7735S, "Panel init failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(*ret_panel);
        return ret;
    }

    ESP_LOGI(TAG_ST7735S, "ST7735S display initialized successfully");
    return ESP_OK;
}
#endif
