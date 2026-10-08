idf_component_register(SRCS "esp_lcd_st7123.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_timer REQUIRES esp_lcd)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_ST7123_VER_MAJOR=1 ESP_LCD_ST7123_VER_MINOR=0 ESP_LCD_ST7123_VER_PATCH=0)
