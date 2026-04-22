#include "ui_display.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "lvgl.h"
#include "font/binfont_loader/lv_binfont_loader.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "USB_NCM_YTMD_ART";

static lv_obj_t *s_album_img = NULL;
static lv_img_dsc_t s_album_dsc;
static uint16_t *s_album_frame = NULL;
static lv_font_t *s_font_small = NULL;
static lv_font_t *s_font_large = NULL;

static bool s_spiffs_mounted = false;
static bool s_font_fs_registered = false;
static lv_fs_drv_t s_font_fs_drv;

#define FONT_PART_LABEL      "storage"
#define FONT_SPIFFS_BASE     "/spiffs"
#define FONT_FS_LETTER       'F'
#define FONT_SMALL_PATH      "F:/font16_sym.bin"
#define FONT_LARGE_PATH      "F:/font27_sym.bin"

static void make_font_full_path(const char *path, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }

    if (!path || path[0] == '\0') {
        out[0] = '\0';
        return;
    }

    const char *rel = (path[0] == '/') ? (path + 1) : path;
    snprintf(out, out_cap, "%s/%s", FONT_SPIFFS_BASE, rel);
}

static void *font_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;
    char full[192] = {0};
    make_font_full_path(path, full, sizeof(full));

    const char *fmode = "rb";
    if ((mode & LV_FS_MODE_RD) && (mode & LV_FS_MODE_WR)) {
        fmode = "rb+";
    } else if (mode & LV_FS_MODE_WR) {
        fmode = "wb";
    }

    return (void *)fopen(full, fmode);
}

static lv_fs_res_t font_fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    if (!file_p) {
        return LV_FS_RES_INV_PARAM;
    }
    return fclose((FILE *)file_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t font_fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    (void)drv;
    if (!file_p || !buf) {
        return LV_FS_RES_INV_PARAM;
    }

    FILE *fp = (FILE *)file_p;
    size_t n = fread(buf, 1, (size_t)btr, fp);
    if (br) {
        *br = (uint32_t)n;
    }
    if (n < (size_t)btr && ferror(fp)) {
        return LV_FS_RES_FS_ERR;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t font_fs_write_cb(lv_fs_drv_t *drv, void *file_p, const void *buf, uint32_t btw, uint32_t *bw)
{
    (void)drv;
    if (!file_p || !buf) {
        return LV_FS_RES_INV_PARAM;
    }

    FILE *fp = (FILE *)file_p;
    size_t n = fwrite(buf, 1, (size_t)btw, fp);
    if (bw) {
        *bw = (uint32_t)n;
    }
    return (n == (size_t)btw) ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t font_fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    (void)drv;
    if (!file_p) {
        return LV_FS_RES_INV_PARAM;
    }

    int c_whence = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) {
        c_whence = SEEK_CUR;
    } else if (whence == LV_FS_SEEK_END) {
        c_whence = SEEK_END;
    }

    return fseek((FILE *)file_p, (long)pos, c_whence) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t font_fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    if (!file_p || !pos_p) {
        return LV_FS_RES_INV_PARAM;
    }

    long pos = ftell((FILE *)file_p);
    if (pos < 0) {
        return LV_FS_RES_FS_ERR;
    }

    *pos_p = (uint32_t)pos;
    return LV_FS_RES_OK;
}

static void register_font_fs_driver(void)
{
    if (s_font_fs_registered) {
        return;
    }

    lv_fs_drv_init(&s_font_fs_drv);
    s_font_fs_drv.letter = FONT_FS_LETTER;
    s_font_fs_drv.cache_size = 0;
    s_font_fs_drv.open_cb = font_fs_open_cb;
    s_font_fs_drv.close_cb = font_fs_close_cb;
    s_font_fs_drv.read_cb = font_fs_read_cb;
    s_font_fs_drv.write_cb = font_fs_write_cb;
    s_font_fs_drv.seek_cb = font_fs_seek_cb;
    s_font_fs_drv.tell_cb = font_fs_tell_cb;
    lv_fs_drv_register(&s_font_fs_drv);

    s_font_fs_registered = true;
    ESP_LOGI(TAG, "LVGL FS driver registered: %c:", FONT_FS_LETTER);
}

static esp_err_t mount_font_spiffs(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = FONT_SPIFFS_BASE,
        .partition_label = FONT_PART_LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount SPIFFS partition '%s': %s", FONT_PART_LABEL, esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info(FONT_PART_LABEL, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS(%s): total=%uKB used=%uKB free=%uKB",
                 FONT_PART_LABEL,
                 (unsigned)(total / 1024),
                 (unsigned)(used / 1024),
                 (unsigned)((total - used) / 1024));
    } else {
        ESP_LOGW(TAG, "esp_spiffs_info failed: %s", esp_err_to_name(err));
    }

    s_spiffs_mounted = true;
    return ESP_OK;
}

static void load_bin_fonts(void)
{
#if LV_USE_BINFONT
    if (!s_font_small) {
        s_font_small = lv_binfont_create(FONT_SMALL_PATH);
        if (s_font_small) {
            ESP_LOGI(TAG, "Loaded font: %s", FONT_SMALL_PATH);
        } else {
            ESP_LOGW(TAG, "Failed to load font: %s", FONT_SMALL_PATH);
        }
    }

    if (!s_font_large) {
        s_font_large = lv_binfont_create(FONT_LARGE_PATH);
        if (s_font_large) {
            ESP_LOGI(TAG, "Loaded font: %s", FONT_LARGE_PATH);
        } else {
            ESP_LOGW(TAG, "Failed to load font: %s", FONT_LARGE_PATH);
        }
    }
#else
    ESP_LOGW(TAG, "LV_USE_BINFONT is disabled; bin fonts will not load");
#endif
}

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

    if (mount_font_spiffs() == ESP_OK) {
        register_font_fs_driver();
        load_bin_fonts();
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
    if (s_font_small) {
        lv_obj_set_style_text_font(scr, s_font_small, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

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

const lv_font_t *ui_display_font_small(void)
{
    return s_font_small;
}

const lv_font_t *ui_display_font_large(void)
{
    return s_font_large;
}
