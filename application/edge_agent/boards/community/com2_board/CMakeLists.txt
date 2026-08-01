/* ESP-Claw ST7735S LCD support for COM2 Board */
idf_component_register(
    SRCS "esp_lcd_st7735s.c"
    INCLUDE_DIRS "." "include"
    REQUIRES "esp_lcd"
)
