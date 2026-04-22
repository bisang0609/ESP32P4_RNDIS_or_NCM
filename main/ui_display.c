#include "ui_display.h"

#include <string.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "USB_NCM_YTMD_ART";

static lv_obj_t *s_album_img = NULL;
static lv_img_dsc_t s_album_dsc;
static uint16_t *s_album_frame = NULL;

esp_err_t ui_display_init(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = false,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };

    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "bsp_display_start_with_config failed");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "bsp_display_backlight_on failed");

    int hor = lv_display_get_horizontal_resolution(disp);
    int ver = lv_display_get_vertical_resolution(disp);
    if (hor < ver) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
    }

    const size_t frame_bytes = (size_t)UI_ALBUM_ART_W * UI_ALBUM_ART_H * 2u;
    s_album_frame = (uint16_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_album_frame) {
        s_album_frame = (uint16_t *)malloc(frame_bytes);
    }
    ESP_RETURN_ON_FALSE(s_album_frame, ESP_ERR_NO_MEM, TAG, "album frame alloc failed");
    memset(s_album_frame, 0, frame_bytes);

    memset(&s_album_dsc, 0, sizeof(s_album_dsc));
    s_album_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_album_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_album_dsc.header.flags = 0;
    s_album_dsc.header.w = UI_ALBUM_ART_W;
    s_album_dsc.header.h = UI_ALBUM_ART_H;
    s_album_dsc.header.stride = UI_ALBUM_ART_W * 2;
    s_album_dsc.data_size = frame_bytes;
    s_album_dsc.data = (const uint8_t *)s_album_frame;

    ESP_RETURN_ON_FALSE(bsp_display_lock(0), ESP_FAIL, TAG, "bsp_display_lock failed");
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_album_img = lv_img_create(scr);
    lv_img_set_src(s_album_img, &s_album_dsc);
    lv_obj_set_size(s_album_img, UI_ALBUM_ART_W, UI_ALBUM_ART_H);
    lv_obj_center(s_album_img);
    bsp_display_unlock();

    (void)bsp_display_brightness_set(100);
    return ESP_OK;
}

uint16_t *ui_display_get_album_buffer(void)
{
    return s_album_frame;
}

void ui_display_present_album_art(void)
{
    if (!s_album_img || !s_album_frame) {
        return;
    }

    if (bsp_display_lock(0)) {
        lv_img_set_src(s_album_img, &s_album_dsc);
        lv_obj_invalidate(s_album_img);
        bsp_display_unlock();
    }
}
