#include "ui_display.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
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
static lv_font_t *s_ttf_font_small = NULL;
static lv_font_t *s_ttf_font_large = NULL;
static lv_font_t s_chain_font_small;
static lv_font_t s_chain_font_large;
static bool s_chain_font_small_ready = false;
static bool s_chain_font_large_ready = false;

#if LV_USE_TINY_TTF
static uint8_t *s_ttf_blob_kr = NULL;
static size_t s_ttf_blob_kr_size = 0;
#endif

static bool s_font_fs_mounted = false;
static bool s_font_fs_registered = false;
static lv_fs_drv_t s_font_fs_drv;

#define FONT_PART_LABEL      "storage"
#define FONT_FS_BASE_PATH    "/spiffs"
#define FONT_FS_LETTER       'F'
#define FONT_SMALL_PATH      "F:/font16_sym.bin"
#define FONT_LARGE_PATH      "F:/font27_sym.bin"
#define FONT_FS_YIELD_CHUNK_BYTES (256u * 1024u)
#define FONT_LOAD_BIN_FALLBACK 0

#define FONT_TTF_KR_PATH_A   "/spiffs/fonts/NotoSansKR-Regular.ttf"
#define FONT_TTF_KR_PATH_B   "/spiffs/NotoSansKR-Regular.ttf"
#define FONT_TTF_TITLE_SIZE  34
#define FONT_TTF_BODY_SIZE   22

static uint32_t s_font_fs_bytes_since_yield = 0;

static const lv_font_t *fallback_small_font(void)
{
#if LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    return &lv_font_source_han_sans_sc_14_cjk;
#elif LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#elif LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#elif LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#elif LV_FONT_MONTSERRAT_12
    return &lv_font_montserrat_12;
#else
    return NULL;
#endif
}

static const lv_font_t *fallback_large_font(void)
{
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#elif LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    return &lv_font_source_han_sans_sc_14_cjk;
#elif LV_FONT_MONTSERRAT_22
    return &lv_font_montserrat_22;
#elif LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_18
    return &lv_font_montserrat_18;
#elif LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#elif LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return NULL;
#endif
}

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
    snprintf(out, out_cap, "%s/%s", FONT_FS_BASE_PATH, rel);
}

#if FONT_LOAD_BIN_FALLBACK
static void make_font_full_path_from_lvfs(const char *lvfs_path, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }

    if (!lvfs_path || lvfs_path[0] == '\0') {
        out[0] = '\0';
        return;
    }

    const char *real = lvfs_path;
    if (real[0] != '\0' && real[1] == ':') {
        real += 2;
    }
    if (real[0] == '/') {
        real += 1;
    }

    snprintf(out, out_cap, "%s/%s", FONT_FS_BASE_PATH, real);
}
#endif

static bool file_exists(const char *path)
{
    if (!path || path[0] == '\0') {
        return false;
    }

    struct stat st = {0};
    return stat(path, &st) == 0;
}

static bool load_file_blob(const char *path, uint8_t **out_blob, size_t *out_size)
{
    if (!path || !out_blob || !out_size) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }

    long file_sz = ftell(fp);
    if (file_sz <= 0) {
        fclose(fp);
        return false;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    uint8_t *blob = (uint8_t *)heap_caps_malloc((size_t)file_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) {
        blob = (uint8_t *)malloc((size_t)file_sz);
    }
    if (!blob) {
        fclose(fp);
        return false;
    }

    size_t n = fread(blob, 1, (size_t)file_sz, fp);
    fclose(fp);

    if (n != (size_t)file_sz) {
        free(blob);
        return false;
    }

    *out_blob = blob;
    *out_size = (size_t)file_sz;
    return true;
}

static void build_font_chain(lv_font_t *out_chain, bool *out_ready, const lv_font_t *base, const lv_font_t *fallback)
{
    if (!out_chain || !out_ready) {
        return;
    }

    *out_ready = false;
    if (!base) {
        return;
    }

    *out_chain = *base;
    if (fallback && fallback != base) {
        out_chain->fallback = fallback;
    }
    *out_ready = true;
}

#if LV_USE_TINY_TTF
static void load_runtime_ttf_fonts(void)
{
    const char *kr_path = NULL;
    if (file_exists(FONT_TTF_KR_PATH_A)) {
        kr_path = FONT_TTF_KR_PATH_A;
    } else if (file_exists(FONT_TTF_KR_PATH_B)) {
        kr_path = FONT_TTF_KR_PATH_B;
    }

    if (!kr_path) {
        ESP_LOGW(TAG, "KR TTF file not found (%s or %s)", FONT_TTF_KR_PATH_A, FONT_TTF_KR_PATH_B);
        return;
    }

    if (!s_ttf_blob_kr && !load_file_blob(kr_path, &s_ttf_blob_kr, &s_ttf_blob_kr_size)) {
        ESP_LOGW(TAG, "Failed to load KR TTF blob: %s", kr_path);
        return;
    }

    if (!s_ttf_font_large) {
        s_ttf_font_large = lv_tiny_ttf_create_data_ex(s_ttf_blob_kr,
                                                      s_ttf_blob_kr_size,
                                                      FONT_TTF_TITLE_SIZE,
                                                      LV_FONT_KERNING_NORMAL,
                                                      0);
    }
    if (!s_ttf_font_small) {
        s_ttf_font_small = lv_tiny_ttf_create_data_ex(s_ttf_blob_kr,
                                                      s_ttf_blob_kr_size,
                                                      FONT_TTF_BODY_SIZE,
                                                      LV_FONT_KERNING_NORMAL,
                                                      0);
    }

    if (s_ttf_font_small && s_ttf_font_large) {
        ESP_LOGI(TAG, "Runtime KR TTF ready: %s (size=%u)", kr_path, (unsigned)s_ttf_blob_kr_size);
    } else {
        ESP_LOGW(TAG, "Runtime KR TTF create failed (small=%p large=%p)", (void *)s_ttf_font_small, (void *)s_ttf_font_large);
    }
}
#endif

static void prepare_font_chains(void)
{
    const lv_font_t *fallback_small = fallback_small_font();
    const lv_font_t *fallback_large = fallback_large_font();

    // Use runtime TTF as base when available so font metrics match rendered glyph size.
    const lv_font_t *base_small = s_ttf_font_small ? (const lv_font_t *)s_ttf_font_small : fallback_small;
    const lv_font_t *base_large = s_ttf_font_large ? (const lv_font_t *)s_ttf_font_large : fallback_large;
    const lv_font_t *fb_small = s_ttf_font_small ? fallback_small : NULL;
    const lv_font_t *fb_large = s_ttf_font_large ? fallback_large : NULL;

    build_font_chain(&s_chain_font_small, &s_chain_font_small_ready, base_small, fb_small);
    build_font_chain(&s_chain_font_large, &s_chain_font_large_ready, base_large, fb_large);
}

#if FONT_LOAD_BIN_FALLBACK
static void probe_font_file(const char *lvfs_path)
{
    if (!lvfs_path || lvfs_path[0] == '\0') {
        return;
    }

    char full[192] = {0};
    make_font_full_path_from_lvfs(lvfs_path, full, sizeof(full));

    struct stat st = {0};
    if (stat(full, &st) == 0) {
        ESP_LOGI(TAG, "Font file OK: %s size=%ld", full, (long)st.st_size);
    } else {
        ESP_LOGW(TAG, "Font file missing/stat fail: %s errno=%d", full, errno);
    }

    lv_fs_file_t file;
    lv_fs_res_t res = lv_fs_open(&file, lvfs_path, LV_FS_MODE_RD);
    if (res != LV_FS_RES_OK) {
        ESP_LOGW(TAG, "LVFS open fail: %s (res=%d)", lvfs_path, (int)res);
        return;
    }

    uint8_t head[8] = {0};
    uint32_t br = 0;
    res = lv_fs_read(&file, head, sizeof(head), &br);
    if (res == LV_FS_RES_OK && br == sizeof(head)) {
        ESP_LOGI(TAG, "LVFS head %s: %02X %02X %02X %02X %02X %02X %02X %02X",
                 lvfs_path, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
    } else {
        ESP_LOGW(TAG, "LVFS read fail: %s (res=%d br=%u)", lvfs_path, (int)res, (unsigned)br);
    }
    lv_fs_close(&file);
}
#endif

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

    // lv_binfont_create can run long enough to starve IDLE0 on P4; yield by transferred bytes.
    s_font_fs_bytes_since_yield += (uint32_t)n;
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        s_font_fs_bytes_since_yield >= FONT_FS_YIELD_CHUNK_BYTES) {
        s_font_fs_bytes_since_yield = 0;
        vTaskDelay(1);
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

static esp_err_t mount_font_fs(void)
{
    if (s_font_fs_mounted) {
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = FONT_FS_BASE_PATH,
        .partition_label = FONT_PART_LABEL,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = true,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount LittleFS partition '%s': %s", FONT_PART_LABEL, esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_littlefs_info(FONT_PART_LABEL, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS(%s): total=%uKB used=%uKB free=%uKB",
                 FONT_PART_LABEL,
                 (unsigned)(total / 1024),
                 (unsigned)(used / 1024),
                 (unsigned)((total - used) / 1024));
    } else {
        ESP_LOGW(TAG, "esp_littlefs_info failed: %s", esp_err_to_name(err));
    }

    s_font_fs_mounted = true;
    return ESP_OK;
}

static void load_bin_fonts(void)
{
#if !FONT_LOAD_BIN_FALLBACK
    ESP_LOGI(TAG, "Binfont fallback disabled; skipping lv_binfont_create");
    return;
#else
    probe_font_file(FONT_SMALL_PATH);
    probe_font_file(FONT_LARGE_PATH);

    if (!s_font_small) {
        int64_t t0 = esp_timer_get_time();
        ESP_LOGI(TAG, "Loading font: %s", FONT_SMALL_PATH);
        s_font_small = lv_binfont_create(FONT_SMALL_PATH);
        int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        if (s_font_small) {
            ESP_LOGI(TAG, "Loaded font: %s (%lld ms)", FONT_SMALL_PATH, (long long)dt_ms);
        } else {
            ESP_LOGW(TAG, "Failed to load font: %s after %lld ms (fallback will be used)",
                     FONT_SMALL_PATH, (long long)dt_ms);
        }
    }

    if (!s_font_large) {
        int64_t t0 = esp_timer_get_time();
        ESP_LOGI(TAG, "Loading font: %s", FONT_LARGE_PATH);
        s_font_large = lv_binfont_create(FONT_LARGE_PATH);
        int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        if (s_font_large) {
            ESP_LOGI(TAG, "Loaded font: %s (%lld ms)", FONT_LARGE_PATH, (long long)dt_ms);
        } else {
            ESP_LOGW(TAG, "Failed to load font: %s after %lld ms (fallback will be used)",
                     FONT_LARGE_PATH, (long long)dt_ms);
        }
    }

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

    if (mount_font_fs() == ESP_OK) {
        register_font_fs_driver();
#if LV_USE_TINY_TTF
        load_runtime_ttf_fonts();
#endif
        load_bin_fonts();
    }
    prepare_font_chains();

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
    const lv_font_t *screen_font = ui_display_font_small();
    if (screen_font) {
        lv_obj_set_style_text_font(scr, screen_font, LV_PART_MAIN | LV_STATE_DEFAULT);
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
    if (s_chain_font_small_ready) {
        return &s_chain_font_small;
    }
    if (s_ttf_font_small) {
        return s_ttf_font_small;
    }
    if (s_font_small) {
        return s_font_small;
    }
    if (s_font_large) {
        return s_font_large;
    }
    return fallback_small_font();
}

const lv_font_t *ui_display_font_large(void)
{
    if (s_chain_font_large_ready) {
        return &s_chain_font_large;
    }
    if (s_ttf_font_large) {
        return s_ttf_font_large;
    }
    if (s_font_large) {
        return s_font_large;
    }
    if (s_font_small) {
        return s_font_small;
    }
    return fallback_large_font();
}

const lv_img_dsc_t *ui_display_get_album_dsc(void)
{
    return s_album_frame ? &s_album_dsc : NULL;
}
