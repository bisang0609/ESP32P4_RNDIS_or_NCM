set(UI_IMAGE_SRCS
    "ui/images/ui_image_like_disenable.c"
    "ui/images/ui_image_like_enable.c"
    "ui/images/ui_image_pause.c"
    "ui/images/ui_image_play.c"
    "ui/images/ui_image_playlist.c"
    "ui/images/ui_image_repeat_all.c"
    "ui/images/ui_image_repeat_disable.c"
    "ui/images/ui_image_repeat_one.c"
    "ui/images/ui_image_sentiment_neutral_disable.c"
    "ui/images/ui_image_sentiment_neutral_enable.c"
    "ui/images/ui_image_shu_disenable.c"
    "ui/images/ui_image_shu_enable.c"
    "ui/images/ui_image_skip_next_.c"
    "ui/images/ui_image_skip_previous.c"
)

idf_component_register(
    SRCS
        "main.c"
        "ncm_net.c"
        "ytmd_client.c"
        "ui_display.c"
        "player_ui.c"
        "ui_font_stubs.c"
        "ui/ui.c"
        "ui/screens.c"
        "ui/images.c"
        "ui/styles.c"
        ${UI_IMAGE_SRCS}
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
        esp_timer
        esp_driver_jpeg
        nvs_flash
        joltwallet__littlefs
        espressif__esp_tinyusb
        espressif__esp32_p4_function_ev_board
        lvgl__lvgl
)

set_source_files_properties(ui/screens.c
    PROPERTIES COMPILE_OPTIONS "-include${CMAKE_CURRENT_LIST_DIR}/ui_screens_compat.h")

# Build and flash the font filesystem image from main/font.
littlefs_create_partition_image(storage font FLASH_IN_PROJECT)
