
idf_component_register(
    SRCS "esp32_p4_function_ev_board.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_i2s esp_driver_sdmmc espressif__esp_codec_dev espressif__esp_lvgl_port lvgl__lvgl
    PRIV_REQUIRES spiffs fatfs espressif__esp_lcd_ek79007 espressif__esp_lcd_ili9881c espressif__esp_lcd_touch_gt911 espressif__esp_lcd_st7701
)
