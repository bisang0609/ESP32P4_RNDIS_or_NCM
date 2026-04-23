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

# main/font 기준으로 LittleFS(storage) 이미지를 생성합니다.
# littlefs_create_partition_image(storage font FLASH_IN_PROJECT)  # `idf.py flash` 시 storage도 함께 플래시하려면 이 줄 사용
littlefs_create_partition_image(storage font)                     # 기본적으로 app만 올리고 storage는 유지하려면 이 줄 사용
