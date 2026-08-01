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
#include "esp_lcd_st7735s.h"
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
static const esp_lcd_st7735s_init_cmd_t st7735s_init_cmds[] = {
    {0xAB, 3, {0x38, 0x80, 0x00}}, // BGR
    {0xBB, 3, {0x2D, 0x0C, 0x02}}, // LC0
    {0xC0, 2, {0x2C, 0x05}}, // VRH1
    {0xC1, 2, {0x05, 0x45}}, // VRH2
    {0xC2, 2, {0x83, 0x40}}, // VRH3
    {0xC3, 2, {0x8A, 0x2A}}, // VCM1
    {0xC4, 2, {0x8A, 0xEE}}, // VCM2
    {0xC5, 2, {0x0E, 0x12}}, // VMH
    {0xC6, 1, {0x0C}}, // VCM_OTP
    {0x3A, 2, {0x05, 0x00}}, // COLMOD: 16-bit
    {0x29, 0, {}}, // Display ON
};

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_dev_handle_t panel_handle;
} custom_lcd_handle_t;

static int lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;

    const dev_display_lcd_config_t *lcd_cfg = (const dev_display_lcd_config_t *)config;

    // Create SPI bus
    const esp_lcd_spi_bus_config_t bus_config = {
        .mosi_io_num = lcd_cfg->io_spi_config.mosi_gpio_num,
        .miso_io_num = lcd_cfg->io_spi_config.miso_gpio_num,
        .sclk_io_num = lcd_cfg->io_spi_config.sclk_gpio_num,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 5120,
    };

    esp_err_t ret = esp_lcd_new_spi_bus(&bus_config, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_LCD, "Failed to create SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create panel IO
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = lcd_cfg->io_spi_config.cs_gpio_num,
        .dc_gpio_num = lcd_cfg->io_spi_config.dc_gpio_num,
        .spi_mode = lcd_cfg->io_spi_config.spi_mode,
        .freq_hz = lcd_cfg->io_spi_config.pclk_hz,
        .spi_device_interface = SPI2_HOST,
    };

    esp_lcd_panel_io_handle_t io_handle;
    ret = esp_lcd_new_panel_io_spi(&io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_LCD, "Failed to create panel IO: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create panel
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = lcd_cfg->lcd_panel_config.reset_gpio_num,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .flags = {
            .reset_active_high = lcd_cfg->lcd_panel_config.flags.reset_active_high,
        },
    };

    esp_lcd_panel_dev_handle_t panel_handle;
    ret = esp_lcd_new_panel_st7735s(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_LCD, "Failed to create ST7735S panel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Send initialization commands
    for (size_t i = 0; i < sizeof(st7735s_init_cmds) / sizeof(st7735s_init_cmds[0]); i++) {
        ret = esp_lcd_panel_io_tx_cmd(panel_handle, st7735s_init_cmds[i].cmd_idx,
                                       st7735s_init_cmds[i].data, st7735s_init_cmds[i].data_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_LCD, "Failed to send init cmd %d: %s", st7735s_init_cmds[i].cmd_idx, esp_err_to_name(ret));
            return ret;
        }
    }

    // Create custom handle
    custom_lcd_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        ESP_LOGE(TAG_LCD, "Failed to allocate LCD handle");
        return ESP_ERR_NO_MEM;
    }
    handle->io_handle = io_handle;
    handle->panel_handle = panel_handle;

    *device_handle = &handle->handle;
    ESP_LOGI(TAG_LCD, "ST7735S LCD initialized: %dx%d at GPIO%d/%d/%d/%d/%d",
             lcd_cfg->x_max, lcd_cfg->y_max,
             lcd_cfg->io_spi_config.cs_gpio_num,
             lcd_cfg->io_spi_config.dc_gpio_num,
             lcd_cfg->io_spi_config.sclk_gpio_num,
             lcd_cfg->io_spi_config.mosi_gpio_num,
             lcd_cfg->lcd_panel_config.reset_gpio_num);

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