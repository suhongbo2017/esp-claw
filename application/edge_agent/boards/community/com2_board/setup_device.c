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

/* ST7735S driver — in components/common/ for guaranteed CMake discovery */
#include "esp_lcd_st7735s.h"

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
 * ST7735S LCD — Custom Device Implementation
 * Uses esp_lcd_st7735s component (components/common/) for panel internals.
 * ================================================================ */

static const char *TAG_LCD = "ST7735S";

/* ── Report configuration back to board-manager ─────────────────────────── */
static const dev_display_lcd_config_t s_lcd_cfg = {
    .name         = "display_lcd",
    .chip         = "st7735",
    .sub_type     = "spi",
    .lcd_width    = 128,
    .lcd_height   = 160,
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
    esp_lcd_panel_handle_t panel = NULL;

    /* Create the panel — all internals handled by esp_lcd_st7735s component */
    ret = esp_lcd_new_panel_st7735s(
        GPIO_NUM_17,   /* MOSI */
        GPIO_NUM_18,   /* SCLK */
        GPIO_NUM_21,   /* CS */
        GPIO_NUM_16,   /* DC */
        GPIO_NUM_15,   /* RST */
        128,           /* width */
        160,           /* height */
        SPI2_HOST,     /* SPI host */
        40 * 1000 * 1000,  /* 40 MHz */
        &panel);
    ESP_GOTO_ON_ERROR(ret, err_out, TAG_LCD, "Panel creation failed");

    /* Register with board manager */
    s_lcd_handles.panel_handle = panel;
    esp_board_device_override_config("display_lcd", &s_lcd_cfg, sizeof(s_lcd_cfg));
    *device_handle = &s_lcd_handles;

    ESP_LOGI(TAG_LCD, "ST7735S ready (%d×%d)", 128, 160);
    return ESP_OK;

err_out:
    return ret;
}

static int display_lcd_deinit(void *device_handle)
{
    dev_display_lcd_handles_t *h = (dev_display_lcd_handles_t *)device_handle;
    if (h != NULL && h->panel_handle != NULL) {
        esp_lcd_panel_del(h->panel_handle);
        h->panel_handle = NULL;
    }
    ESP_LOGI(TAG_LCD, "ST7735S deinitialised");
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);
