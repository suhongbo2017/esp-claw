/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_http_server.h"

esp_err_t http_server_register_ota_routes(httpd_handle_t server);
