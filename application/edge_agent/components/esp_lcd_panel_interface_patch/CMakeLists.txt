# Provide esp_lcd_panel_t struct definition for board-level setup_device.c
# This is a local copy of ESP-IDF's internal esp_lcd_panel_interface.h.
# In ESP-IDF v5.x, the struct is opaque to application code; boards that need
# to populate a panel vtable must define it themselves with exact layout match.

idf_component_register(
    INCLUDE_DIRS "include"
)
