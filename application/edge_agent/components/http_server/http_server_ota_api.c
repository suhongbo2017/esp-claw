/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_ota_api.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_update.h"
#include "esp_log.h"
#include "esp partition.h"
#include "http_server.h"

static const char *TAG = "http_ota";

typedef struct {
    esp_http_update_handle_t update_handle;
    esp_http_update_config_t config;
    bool is_updating;
    char error_msg[256];
} ota_ctx_t;

static ota_ctx_t s_ota_ctx = {0};

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ota_supported", true);
    cJSON_AddBoolToObject(root, "ota_in_progress", s_ota_ctx.is_updating);

    if (s_ota_ctx.is_updating) {
        char status_msg[128];
        snprintf(status_msg, sizeof(status_msg), "OTA update in progress: %s",
                 s_ota_ctx.error_msg[0] ? s_ota_ctx.error_msg : "running...");
        http_server_json_add_string(root, "status", status_msg);
    } else {
        http_server_json_add_string(root, "status", "idle");
    }

    return http_server_send_json_response(req, root);
}

static esp_err_t ota_abort_handler(httpd_req_t *req)
{
    if (!s_ota_ctx.is_updating) {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        http_server_json_add_string(root, "error", "No OTA update in progress");
        return http_server_send_json_response(req, root);
    }

    ESP_LOGI(TAG, "OTA update aborted by user");

    if (s_ota_ctx.update_handle) {
        esp_http_update_abort(s_ota_ctx.update_handle);
        esp_http_update_free(s_ota_ctx.update_handle);
        s_ota_ctx.update_handle = NULL;
    }

    s_ota_ctx.is_updating = false;
    s_ota_ctx.error_msg[0] = '\0';

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "message", "OTA update aborted");

    return http_server_send_json_response(req, root);
}

static esp_err_t ota_start_handler(httpd_req_t *req)
{
    if (s_ota_ctx.is_updating) {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        http_server_json_add_string(root, "error", "OTA update already in progress");
        return http_server_send_json_response(req, root);
    }

    cJSON *root = NULL;
    esp_err_t err = http_server_parse_json_body(req, &root);
    if (err != ESP_OK || !root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "url"));
    if (!url || !url[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'url' field");
    }

    ESP_LOGI(TAG, "Starting OTA update from: %s", url);

    memset(&s_ota_ctx.config, 0, sizeof(s_ota_ctx.config));
    s_ota_ctx.config.url = url;
    s_ota_ctx.config.cert_pem = NULL;

    s_ota_ctx.update_handle = esp_http_update_init(&s_ota_ctx.config);
    if (!s_ota_ctx.update_handle) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to initialize HTTP update");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to initialize OTA");
        return ESP_ERR_INVALID_STATE;
    }

    s_ota_ctx.is_updating = true;
    s_ota_ctx.error_msg[0] = '\0';

    // Start OTA update in a task
    xTaskCreatePinnedToCore(ota_update_task, "ota_update", 8192, NULL, 5, NULL, 1);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "message", "OTA update started");
    cJSON_Delete(root);

    return http_server_send_json_response(req, resp);
}

static void ota_update_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "OTA update task started");

    esp_http_update_event_t event;
    esp_event_base_t update_event_base = ESP_HTTP_UPDATE_EVENT;

    while (s_ota_ctx.is_updating && s_ota_ctx.update_handle) {
        if (esp_http_update_event_poll(s_ota_ctx.update_handle, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (event.event_id) {
                case HTTP_UPDATE_EVENT_CONNECTED:
                    ESP_LOGI(TAG, "Connected to server");
                    snprintf(s_ota_ctx.error_msg, sizeof(s_ota_ctx.error_msg), "Connected to server");
                    break;

                case HTTP_UPDATE_EVENT_HEADER_SENT:
                    ESP_LOGI(TAG, "Header sent");
                    snprintf(s_ota_ctx.error_msg, sizeof(s_ota_ctx.error_msg), "Downloading firmware...");
                    break;

                case HTTP_UPDATE_EVENT_HEADER_RECEIVED:
                    ESP_LOGI(TAG, "Header received");
                    break;

                case HTTP_UPDATE_EVENT_DATA_RECEIVED:
                    ESP_LOGI(TAG, "Data received: %d bytes", event.data_received);
                    break;

                case HTTP_UPDATE_EVENT_ENHANCED_CONNECTED:
                    ESP_LOGI(TAG, "Enhanced connection established");
                    break;

                case HTTP_UPDATE_EVENT_CONNECTING:
                    ESP_LOGW(TAG, "Connecting to server...");
                    snprintf(s_ota_ctx.error_msg, sizeof(s_ota_ctx.error_msg), "Connecting...");
                    break;

                case HTTP_UPDATE_EVENT_ERROR:
                    ESP_LOGE(TAG, "Error: %d", event.error_id);
                    snprintf(s_ota_ctx.error_msg, sizeof(s_ota_ctx.error_msg), "Error: %d", event.error_id);
                    s_ota_ctx.is_updating = false;
                    break;

                case HTTP_UPDATE_EVENT_FINISHED:
                    ESP_LOGI(TAG, "Firmware download finished");
                    snprintf(s_ota_ctx.error_msg, sizeof(s_ota_ctx.error_msg), "Download complete, verifying...");
                    break;

                default:
                    break;
            }
        }
    }

    if (s_ota_ctx.is_updating && s_ota_ctx.update_handle) {
        // Check if update was successful
        esp_err_t err = esp_http_update_finish(s_ota_ctx.update_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA update successful, rebooting...");
            httpd_resp_send_200(NULL); // Send response before reboot

            // Set the new partition as boot partition
            const esp_partition_t *boot_partition = esp_partition_get_next(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
            if (boot_partition) {
                esp_err_t ret = esp_partition_set_boot(boot_partition, true);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(ret));
                }
            }

            // Reboot after a short delay
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
            snprintf(s_ota_ctx.error_msg, sizeof(s_ota_ctx.error_msg), "Update failed: %s", esp_err_to_name(err));
        }
    }

    s_ota_ctx.is_updating = false;
    s_ota_ctx.error_msg[0] = '\0';

    if (s_ota_ctx.update_handle) {
        esp_http_update_free(s_ota_ctx.update_handle);
        s_ota_ctx.update_handle = NULL;
    }

    ESP_LOGI(TAG, "OTA update task finished");
    vTaskDelete(NULL);
}

esp_err_t http_server_register_ota_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_handler },
        { .uri = "/api/ota/start", .method = HTTP_POST, .handler = ota_start_handler },
        { .uri = "/api/ota/abort", .method = HTTP_POST, .handler = ota_abort_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGI(TAG, "OTA routes registered");
    return ESP_OK;
}
