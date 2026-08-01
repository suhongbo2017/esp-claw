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
static const char *TAG_LCD = "COM2_BOARD_LCD_INIT";

// ST7735S initialization commands
static const struct {
    uint8_t cmd;
    const uint8_t *data;
    size_t len;
    uint16_t delay_ms;
} st7735s_init_cmds[] = {
    {0xAB, (const uint8_t[]){0x38, 0x80, 0x00}, 3, 0},   // BGR
    {0xBB, (const uint8_t[]){0x2D, 0x0C, 0x02}, 3, 0},   // LC0
    {0xC0, (const uint8_t[]){0x2C, 0x05}, 2, 0},         // VRH1
    {0xC1, (const uint8_t[]){0x05, 0x45}, 2, 0},         // VRH2
    {0xC2, (const uint8_t[]){0x83, 0x40}, 2, 0},         // VRH3
    {0xC3, (const uint8_t[]){0x8A, 0x2A}, 2, 0},         // VCM1
    {0xC4, (const uint8_t[]){0x8A, 0xEE}, 2, 0},         // VCM2
    {0xC5, (const uint8_t[]){0x0E, 0x12}, 2, 0},         // VMH
    {0xC6, (const uint8_t[]){0x0C}, 1, 0},               // VCM_OTP
    {0x3A, (const uint8_t[]){0x05, 0x00}, 2, 0},         // COLMOD: 16-bit
    {0x29, NULL, 0, 120},                                 // Display ON
};

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_dev_handle_t panel_handle;
} custom_lcd_handle_t;

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG_LCD, "Creating ST7735S panel");

    // Note: esp_lcd_new_panel_st7735s may not be available in all ESP-IDF versions.
    // For now, we'll use a placeholder - the actual panel creation should be done
    // by the board manager or a compatible driver.
    // This is a simplified implementation that assumes the panel driver is available.

    // Send initialization commands via panel IO
    for (size_t i = 0; i < sizeof(st7735s_init_cmds) / sizeof(st7735s_init_cmds[0]); i++) {
        esp_err_t ret = esp_lcd_panel_io_tx_param(io, st7735s_init_cmds[i].cmd,
                                                    st7735s_init_cmds[i].data,
                                                    st7735s_init_cmds[i].len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_LCD, "Failed to send init cmd 0x%02x: %s", st7735s_init_cmds[i].cmd, esp_err_to_name(ret));
            return ret;
        }
        if (st7735s_init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(st7735s_init_cmds[i].delay_ms));
        }
    }

    ESP_LOGI(TAG_LCD, "ST7735S initialization commands sent");
    return ESP_OK;
}

static int lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;

    custom_lcd_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        ESP_LOGE(TAG_LCD, "Failed to allocate LCD handle");
        return ESP_ERR_NO_MEM;
    }

    *device_handle = &handle->handle;
    ESP_LOGI(TAG_LCD, "ST7735S LCD device initialized");
    return ESP_OK;
}

static int lcd_deinit(void *device_handle)
{
    if (device_handle) {
        custom_lcd_handle_t *handle = (custom_lcd_handle_t *)device_handle;
        if (handle->panel_handle) {
            esp_lcd_delete_panel(handle->panel_handle);
        }
        if (handle->io_handle) {
            esp_lcd_delete_panel_io(handle->io_handle);
        }
        free(handle);
    }
    ESP_LOGI(TAG_LCD, "ST7735S LCD deinitialized");
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, lcd_init, lcd_deinit);
#endif