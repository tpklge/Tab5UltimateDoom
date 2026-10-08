idf_component_register(SRCS "main.c" "bsp_tab5.c" "doomEsp.c" "doomEsp_sound.c"
                       PRIV_REQUIRES esp_lcd esp_lcd_touch esp_driver_ppa esp_timer esp_lcd_ili9881c esp_lcd_st7123 esp_lcd_touch_gt911 esp_io_expander_pi4ioe5v6408 spiffs doomgeneric vfs usb esp_codec_dev driver fatfs esp_mm
                       INCLUDE_DIRS "")
