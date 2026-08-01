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
 * ST7735S LCD Driver — Custom Device Implementation
 *
 * Pattern modelled on waveshare_rlcd_4_2/setup_device.c.
 * Since ST7735S has no built-in driver in ESP-IDF, we must:
 *   1. Manually initialise the SPI bus
 *   2. Create panel IO via esp_lcd_new_panel_io_spi()
 *   3. Allocate our own panel context (with embedded base struct)
 *   4. Populate vtable functions directly
 *   5. Call esp_lcd_panel_reset() + esp_lcd_panel_init() ourselves
 *   6. Register via esp_board_device_override_config() +
 *      CUSTOM_DEVICE_IMPLEMENT(display_lcd, ...)
 * ================================================================ */

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
static const char *TAG_ST7735S = "ST7735S";

/* ── Pin assignments ─────────────────────────────────────────────────────── */
#define LCD_MOSI_GPIO  17
#define LCD_SCK_GPIO   18
#define LCD_CS_GPIO    21
#define LCD_DC_GPIO    16
#define LCD_RST_GPIO   15

/* ── Display geometry ────────────────────────────────────────────────────── */
#define LCD_WIDTH      128
#define LCD_HEIGHT     160

/* ── SPI clock ───────────────────────────────────────────────────────────── */
#define LCD_SPI_HOST   SPI2_HOST
#define LCD_SPI_CLK_HZ (40 * 1000 * 1000)  /* 40 MHz */

/* ── Panel context ───────────────────────────────────────────────────────── */
typedef struct {
    esp_lcd_panel_t base;           /* VTABLE — MUST BE FIRST */
    esp_lcd_panel_io_handle_t io;
    gpio_num_t rst_gpio;
    uint8_t madctl;                 /* Memory Access Control byte */
} st7735s_panel_t;

/* ── Init-command table ──────────────────────────────────────────────────── */
#define LCD_INIT_MAX_DATA 16

typedef struct {
    uint8_t cmd;
    uint8_t data[LCD_INIT_MAX_DATA];
    uint8_t len;
    uint32_t delay_ms;
} lcd_init_cmd_t;

/*
 * Initialisation sequence derived from the ST7735S datasheet and verified
 * against the working 7_spi_lcd reference implementation.
 */
static const lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x01, {}, 0, 20},                          /* Software Reset */
    {0x11, {}, 0, 120},                         /* Sleep Out */
    {0xB1, {0x00, 0x1B, 0x08}, 3, 0},          /* Frame Rate Ctrl */
    {0xC0, {0x0D, 0x01, 0x02, 0x02}, 4, 0},    /* Display Inversion Ctrl */
    {0xC1, {0x40, 0x81}, 2, 0},                /* Display Inversion Ctrl (2) */
    {0xC5, {0x30, 0x30}, 2, 0},                /* VCOM Control */
    {0xC8, {0x00,0x32,0x36,0x3D,0x3E,0x2C,0x29,0x2E,0x30,0x30,0x38,0x3B}, 12, 0}, /* Gamma */
    {0x3A, {0x55}, 1, 0},                      /* Pixel Format: 16-bit RGB565 */
    {0x2A, {0x00, 0x00, 0x00, 0x7F}, 4, 0},    /* Column Address Set */
    {0x2B, {0x00, 0x00, 0x00, 0x9F}, 4, 0},    /* Page Address Set */
    {0x29, {}, 0, 120},                        /* Display On */
};

/* Send init commands (MADCTL injected at 0x36) */
static esp_err_t send_init_cmds(esp_lcd_panel_io_handle_t io, uint8_t madctl)
{
    for (size_t i = 0; i < sizeof(s_lcd_init_cmds)/sizeof(s_lcd_init_cmds[0]); i++) {
        const lcd_init_cmd_t *c = &s_lcd_init_cmds[i];

        const uint8_t *buf = c->data;
        size_t len = c->len;

        /* Inject MADCTL at command 0x36 (Memory Access Control) */
        uint8_t local_buf[1] = { madctl };
        if (c->cmd == 0x36 && c->len == 0) {
            buf = local_buf;
            len = 1;
        }

        esp_err_t ret = esp_lcd_panel_io_tx_param(io, c->cmd, buf, len);
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

/* ── Callbacks ───────────────────────────────────────────────────────────── */

static esp_err_t cb_del(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = (st7735s_panel_t *)panel;
    if (p->rst_gpio >= 0) {
        gpio_reset_pin(p->rst_gpio);
    }
    free(p);
    return ESP_OK;
}

static esp_err_t cb_reset(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = (st7735s_panel_t *)panel;
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
    st7735s_panel_t *p = (st7735s_panel_t *)panel;
    ESP_LOGI(TAG_ST7735S, "Sending init seq (MADCTL=0x%02X)", p->madctl);
    return send_init_cmds(p->io, p->madctl);
}

static esp_err_t cb_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                 int x_end, int y_end, const void *color_data)
{
    st7735s_panel_t *p = (st7735s_panel_t *)panel;

    uint8_t caset[4] = {
        (uint8_t)(x_start >> 8), (uint8_t)x_start,
        (uint8_t)((x_end - 1) >> 8), (uint8_t)(x_end - 1)
    };
    uint8_t raset[4] = {
        (uint8_t)(y_start >> 8), (uint8_t)y_start,
        (uint8_t)((y_end - 1) >> 8), (uint8_t)(y_end - 1)
    };

    esp_lcd_panel_io_tx_param(p->io, 0x2A, caset, 4);  /* CASET */
    esp_lcd_panel_io_tx_param(p->io, 0x2B, raset, 4);  /* RASET */
    esp_lcd_panel_io_tx_param(p->io, 0x2C, NULL, 0);   /* RAMWR */

    size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * 2;
    esp_lcd_panel_io_tx_param(p->io, 0x2C, color_data, len);
    return ESP_OK;
}

static esp_err_t cb_mirror(esp_lcd_panel_t *panel, bool mx, bool my)
{
    st7735s_panel_t *p = (st7735s_panel_t *)panel;
    uint8_t v = p->madctl;
    if (mx) v |= 0x40; else v &= ~0x40;
    if (my) v |= 0x20; else v &= ~0x20;
    return esp_lcd_panel_io_tx_param(p->io, 0x36, &v, 1);
}

static esp_err_t cb_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    st7735s_panel_t *p = (st7735s_panel_t *)panel;
    uint8_t v = p->madctl;
    if (swap) v |= 0x10; else v &= ~0x10;
    return esp_lcd_panel_io_tx_param(p->io, 0x36, &v, 1);
}

static esp_err_t cb_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    st7735s_panel_t *p = (st7735s_panel_t *)panel;
    uint8_t cmd = invert ? 0x21 : 0x20;
    return esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
}

static esp_err_t cb_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    st7735s_panel_t *p = (st7735s_panel_t *)panel;
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
    st7735s_panel_t *p = (st7735s_panel_t *)panel;
    uint8_t cmd = sleep ? 0x10 : 0x11;
    uint32_t delay = sleep ? 120 : 0;
    esp_err_t ret = esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
    if (!sleep && ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
    return ret;
}

/* ── Report configuration back to board-manager ──────────────────────────── */
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
                        TAG_ST7735S, "null handle");

    esp_err_t ret = ESP_OK;
    st7735s_panel_t *p = NULL;

    /* Compute MADCTL: mirror_x=false, mirror_y=true → MY=1 (0x20) */
    uint8_t madctl = 0x20;  /* Default portrait orientation from board yaml */

    /* ── 1. Configure RST GPIO ──────────────────────────────────────── */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(LCD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type  = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&rst_cfg);
    ESP_GOTO_ON_ERROR(ret, err_out, TAG_ST7735S, "RST GPIO config failed");

    /* ── 2. Initialise SPI bus ──────────────────────────────────────── */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = LCD_MOSI_GPIO,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 10,  /* 10 lines of RGB565 */
    };
    ret = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    ESP_GOTO_ON_ERROR(ret, err_free_spi, TAG_ST7735S, "SPI bus init failed");

    /* ── 3. Create panel IO (SPI) ───────────────────────────────────── */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num  = LCD_DC_GPIO,
        .cs_gpio_num  = LCD_CS_GPIO,
        .pclk_hz      = LCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode     = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                    &io_cfg, &io_handle);
    ESP_GOTO_ON_ERROR(ret, err_free_spi, TAG_ST7735S, "panel IO init failed");

    /* ── 4. Allocate panel context ──────────────────────────────────── */
    p = calloc(1, sizeof(*p));
    ESP_GOTO_ON_FALSE(p != NULL, ESP_ERR_NO_MEM, err_free_io, TAG_ST7735S,
                      "panel alloc failed");

    p->io     = io_handle;
    p->rst_gpio = (gpio_num_t)LCD_RST_GPIO;
    p->madctl = madctl;

    /* ── 5. Populate vtable ─────────────────────────────────────────── */
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

    /* ── 6. Reset & initialze panel ─────────────────────────────────── */
    ret = esp_lcd_panel_reset(&p->base);
    ESP_GOTO_ON_ERROR(ret, err_free_panel, TAG_ST7735S, "panel reset failed");

    ret = esp_lcd_panel_init(&p->base);
    ESP_GOTO_ON_ERROR(ret, err_free_panel, TAG_ST7735S, "panel init failed");

    /* ── 7. Register with board manager ─────────────────────────────── */
    s_lcd_handles.io_handle   = io_handle;
    s_lcd_handles.panel_handle = &p->base;
    esp_board_device_override_config("display_lcd", &s_lcd_cfg, sizeof(s_lcd_cfg));
    *device_handle = &s_lcd_handles;

    ESP_LOGI(TAG_ST7735S, "ST7735S ready (%d×%d @ %d MHz)",
             LCD_WIDTH, LCD_HEIGHT, LCD_SPI_CLK_HZ / 1000000);
    return ESP_OK;

err_free_panel:
    free(p);
err_free_io:
    esp_lcd_panel_io_del(io_handle);
err_free_spi:
    spi_bus_free(LCD_SPI_HOST);
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
        if (h->io_handle != NULL) {
            esp_lcd_panel_io_del(h->io_handle);
            h->io_handle = NULL;
        }
    }
    spi_bus_free(LCD_SPI_HOST);
    ESP_LOGI(TAG_ST7735S, "ST7735S deinitialised");
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);
#endif  /* CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT */
