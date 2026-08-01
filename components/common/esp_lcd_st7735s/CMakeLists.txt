# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS
        "src/esp_lcd_st7735s.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_lcd
)
