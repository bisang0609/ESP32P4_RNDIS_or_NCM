idf_component_register(
    SRCS
        "main.c"
        "ncm_net.c"
        "ytmd_client.c"
        "ui_display.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_client
        esp-tls
        esp_netif
        esp_event
        spi_flash
        esp_psram
        esp_partition
        app_update
        bootloader_support
        esp_driver_jpeg
        spiffs
        nvs_flash
        espressif__esp_tinyusb
        espressif__esp32_p4_function_ev_board
        lvgl__lvgl
)

spiffs_create_partition_image(storage font FLASH_IN_PROJECT)
