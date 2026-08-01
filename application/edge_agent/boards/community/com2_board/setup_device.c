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

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
/* ================================================================
 * ST7735S LCD Driver - Inlined for com2_board
 * ================================================================ */
static const char *TAG_ST7735S = "ST7735S";

/* ST7735S initialization commands */
static const struct {
    uint8_t cmd;
    const uint8_t *data;
    size_t len;
    uint16_t delay_ms;
} st7735s_init_cmds[] = {
    /* Soft reset */
    {0x01, NULL, 0, 5},
    /* Sleep out */
    {0x11, NULL, 0, 120},
    /* Frame Rate Control */
    {0xB1, (const uint8_t[]){0x00, 0x1B, 0x08}, 3, 0},
    /* Display Inversion Control */
    {0xC0, (const uint8_t[]){0x0D, 0x01, 0x02, 0x02}, 4, 0},
    {0xC1, (const uint8_t[]){0x40, 0x81}, 2, 0},
    /* Exit Partial Mode */
    {0xC5, (const uint8_t[]){0x30, 0x30}, 2, 0},
    /* Gamma Control */
    {0xC8, (const uint8_t[]){0x00, 0x32, 0x36, 0x3D, 0x3E, 0x2C, 0x29, 0x2E, 0x30, 0x30, 0x38, 0x3B}, 12, 0},
    /* Pixel Format: 16-bit */
    {0x3A, (const uint8_t[]){0x55}, 1, 0},
    /* Column Address Set */
    {0x2A, (const uint8_t[]){0x00, 0x00, 0x00, 0x7F}, 4, 0},
    /* Page Address Set */
    {0x2B, (const uint8_t[]){0x00, 0x00, 0x00, 0x9F}, 4, 0},
    /* Display On */
    {0x29, NULL, 0, 120},
};

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
} st7735s_panel_priv_t;

static esp_err_t st7735s_del_cb(esp_lcd_panel_t *panel)
{
    st7735s_panel_priv_t *priv = panel->user_data;
    if (priv->reset_gpio_num >= 0) {
        gpio_reset_pin(priv->reset_gpio_num);
    }
    free(priv);
    return ESP_OK;
}

static esp_err_t st7735s_reset_cb(esp_lcd_panel_t *panel)
{
    st7735s_panel_priv_t *priv = panel->user_data;
    if (priv->reset_gpio_num >= 0) {
        gpio_set_level(priv->reset_gpio_num, !priv->reset_level);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(priv->reset_gpio_num, priv->reset_level);
        vTaskDelay(pdMS_TO_TICKS(5));
    } else {
        esp_lcd_panel_io_tx_param(priv->io, 0x01, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

static esp_err_t st7735s_init_cb(esp_lcd_panel_t *panel)
{
    st7735s_panel_priv_t *priv = panel->user_data;
    for (size_t i = 0; i < sizeof(st7735s_init_cmds) / sizeof(st7735s_init_cmds[0]); i++) {
        if (st7735s_init_cmds[i].len > 0) {
            esp_lcd_panel_io_tx_param(priv->io, st7735s_init_cmds[i].cmd,
                                       st7735s_init_cmds[i].data,
                                       st7735s_init_cmds[i].len);
        } else {
            esp_lcd_panel_io_tx_param(priv->io, st7735s_init_cmds[i].cmd, NULL, 0);
        }
        if (st7735s_init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(st7735s_init_cmds[i].delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t st7735s_draw_bitmap_cb(esp_lcd_panel_t *panel, int x_start, int y_start,
                                         int x_end, int y_end, const void *color_data)
{
    (void)panel; (void)x_start; (void)y_start;
    (void)x_end; (void)y_end; (void)color_data;
    return ESP_OK;
}

static esp_err_t st7735s_invert_color_cb(esp_lcd_panel_t *panel, bool invert)
{
    st7735s_panel_priv_t *priv = panel->user_data;
    return esp_lcd_panel_io_tx_param(priv->io, invert ? 0x20 : 0x21, NULL, 0);
}

static esp_err_t st7735s_mirror_cb(esp_lcd_panel_t *panel, bool mx, bool my)
{
    st7735s_panel_priv_t *priv = panel->user_data;
    uint8_t madctl_val = 0;
    esp_lcd_panel_io_rx_param(priv->io, 0x36, &madctl_val, 1);
    if (mx) madctl_val |= 0x40; else madctl_val &= ~0x40;
    if (my) madctl_val |= 0x20; else madctl_val &= ~0x20;
    return esp_lcd_panel_io_tx_param(priv->io, 0x36, &madctl_val, 1);
}

static esp_err_t st7735s_swap_xy_cb(esp_lcd_panel_t *panel, bool swap)
{
    st7735s_panel_priv_t *priv = panel->user_data;
    uint8_t madctl_val = 0;
    esp_lcd_panel_io_rx_param(priv->io, 0x36, &madctl_val, 1);
    if (swap) madctl_val |= 0x10; else madctl_val &= ~0x10;
    return esp_lcd_panel_io_tx_param(priv->io, 0x36, &madctl_val, 1);
}

static esp_err_t st7735s_set_gap_cb(esp_lcd_panel_t *panel, int x, int y)
{
    (void)panel; (void)x; (void)y;
    return ESP_OK;
}

static esp_err_t st7735s_disp_on_off_cb(esp_lcd_panel_t *panel, bool on)
{
    st7735s_panel_priv_t *priv = panel->user_data;
    return esp_lcd_panel_io_tx_param(priv->io, on ? 0x29 : 0x28, NULL, 0);
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG_ST7735S, "Creating ST7735S panel via factory");

    /* Allocate private data */
    st7735s_panel_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        ESP_LOGE(TAG_ST7735S, "Failed to allocate panel private data");
        return ESP_ERR_NO_MEM;
    }

    priv->io = io;
    priv->reset_gpio_num = panel_dev_config->reset_gpio_num;
    priv->reset_level = panel_dev_config->flags.reset_active_high;

    /* Allocate panel structure */
    esp_lcd_panel_t *panel = calloc(1, sizeof(*panel));
    if (!panel) {
        free(priv);
        ESP_LOGE(TAG_ST7735S, "Failed to allocate panel structure");
        return ESP_ERR_NO_MEM;
    }

    /* Register callbacks */
    panel->del           = st7735s_del_cb;
    panel->init          = st7735s_init_cb;
    panel->reset         = st7735s_reset_cb;
    panel->draw_bitmap   = st7735s_draw_bitmap_cb;
    panel->invert_color  = st7735s_invert_color_cb;
    panel->mirror        = st7735s_mirror_cb;
    panel->swap_xy       = st7735s_swap_xy_cb;
    panel->set_gap       = st7735s_set_gap_cb;
    panel->disp_on_off   = st7735s_disp_on_off_cb;
    panel->user_data     = priv;

    *ret_panel = (esp_lcd_panel_handle_t)panel;
    ESP_LOGI(TAG_ST7735S, "ST7735S panel created successfully @%p", panel);
    return ESP_OK;
}
#endif
