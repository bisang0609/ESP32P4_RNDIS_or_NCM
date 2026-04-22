#include "ytmd_client.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "driver/jpeg_decode.h"

static const char *TAG = "USB_NCM_YTMD_ART";

#define YTMD_URL_API         "http://192.168.137.1:26538/api/v1/song"
#define HTTP_TIMEOUT_MS      7000
#define MAX_JSON_BYTES       (64 * 1024)
#define MAX_IMAGE_BYTES      (3 * 1024 * 1024)

#define ART_SRC_REQ_W        400
#define ART_SRC_REQ_H        400
#define YTIMG_FALLBACK_FILE  "sddefault.jpg"

static jpeg_decoder_handle_t s_jpeg_decoder = NULL;

static void append_cstr(char *dst, size_t cap, const char *suffix)
{
    if (!dst || !suffix || cap == 0) {
        return;
    }
    size_t len = strlen(dst);
    if (len >= cap - 1) {
        return;
    }
    snprintf(dst + len, cap - len, "%s", suffix);
}

static char *dup_cstr(const char *src)
{
    if (!src) {
        return NULL;
    }
    size_t n = strlen(src);
    char *dst = (char *)malloc(n + 1);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, n + 1);
    return dst;
}

static void replace_all(char *buf, size_t cap, const char *from, const char *to)
{
    if (!buf || !from || !to) {
        return;
    }

    const size_t from_len = strlen(from);
    const size_t to_len = strlen(to);
    if (from_len == 0) {
        return;
    }

    char *pos = strstr(buf, from);
    while (pos) {
        size_t buf_len = strlen(buf);
        size_t tail_len = strlen(pos + from_len);

        if (to_len > from_len) {
            size_t grow = to_len - from_len;
            if (buf_len + grow + 1 > cap) {
                break;
            }
        }

        memmove(pos + to_len, pos + from_len, tail_len + 1);
        memcpy(pos, to, to_len);
        pos = strstr(pos + to_len, from);
    }
}

static void normalize_art_url(char *url, size_t cap)
{
    if (!url || cap == 0) {
        return;
    }

    if (strstr(url, "googleusercontent.com") || strstr(url, "ytimg.com") || strstr(url, "ggpht.com")) {
        char *eq = strrchr(url, '=');
        if (eq && eq[1] && (eq[1] == 'w' || eq[1] == 's')) {
            char size_suffix[32] = {0};
            snprintf(size_suffix, sizeof(size_suffix), "=w%d-h%d", ART_SRC_REQ_W, ART_SRC_REQ_H);
            *eq = '\0';
            append_cstr(url, cap, size_suffix);
        }
    }

    if (strstr(url, "i.ytimg.com/vi/")) {
        replace_all(url, cap, "/maxresdefault.jpg", "/" YTIMG_FALLBACK_FILE);
        replace_all(url, cap, "/hq720.jpg", "/" YTIMG_FALLBACK_FILE);
        replace_all(url, cap, "/hq720_live.jpg", "/" YTIMG_FALLBACK_FILE);
        replace_all(url, cap, "/hqdefault.jpg", "/" YTIMG_FALLBACK_FILE);
        replace_all(url, cap, "/mqdefault.jpg", "/" YTIMG_FALLBACK_FILE);
    }
}

static bool is_jpeg_data(const uint8_t *buf, size_t len)
{
    if (!buf || len < 3) {
        return false;
    }
    return (buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF);
}

static bool make_ytimg_fallback_url(const char *video_id, char *out, size_t cap)
{
    if (!video_id || !out || cap < 40) {
        return false;
    }

    size_t n = strlen(video_id);
    if (n == 0 || n > 32) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)video_id[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
    }

    int w = snprintf(out, cap, "https://i.ytimg.com/vi/%s/%s", video_id, YTIMG_FALLBACK_FILE);
    return (w > 0 && (size_t)w < cap);
}

static bool is_valid_video_id_char(char c)
{
    unsigned char u = (unsigned char)c;
    return (isalnum(u) || u == '_' || u == '-');
}

static bool is_valid_youtube_video_id(const char *s, size_t n)
{
    if (!s || n != 11) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!is_valid_video_id_char(s[i])) {
            return false;
        }
    }
    return true;
}

static char *dup_video_id_if_valid(const char *candidate)
{
    if (!candidate) {
        return NULL;
    }
    size_t n = strlen(candidate);
    if (!is_valid_youtube_video_id(candidate, n)) {
        return NULL;
    }
    return dup_cstr(candidate);
}

static bool extract_video_id_from_watch_like_url(const char *url, char *out, size_t out_cap)
{
    if (!url || !out || out_cap < 12) {
        return false;
    }

    const char *p = strstr(url, "watch?v=");
    if (p) {
        p += strlen("watch?v=");
    } else if ((p = strstr(url, "youtu.be/")) != NULL) {
        p += strlen("youtu.be/");
    } else if ((p = strstr(url, "/shorts/")) != NULL) {
        p += strlen("/shorts/");
    } else {
        return false;
    }

    size_t n = 0;
    while (p[n] && p[n] != '&' && p[n] != '?' && p[n] != '/' && n < 11) {
        n++;
    }
    if (!is_valid_youtube_video_id(p, n)) {
        return false;
    }

    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static char *extract_quoted_json_value_after_key(const char *json, const char *key_token)
{
    if (!json || !key_token) {
        return NULL;
    }

    const char *p = json;
    while ((p = strstr(p, key_token)) != NULL) {
        const char *colon = strchr(p + strlen(key_token), ':');
        if (!colon) {
            break;
        }
        const char *v = colon + 1;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') {
            v++;
        }
        if (*v != '"') {
            p = v;
            continue;
        }
        v++;

        char *out = (char *)malloc(YTMD_ART_URL_MAX_LEN);
        if (!out) {
            return NULL;
        }

        size_t n = 0;
        bool esc = false;
        while (*v) {
            char c = *v++;
            if (esc) {
                if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                    out[n++] = c;
                }
                esc = false;
                continue;
            }
            if (c == '\\') {
                esc = true;
                continue;
            }
            if (c == '"') {
                break;
            }
            if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                out[n++] = c;
            }
        }
        out[n] = '\0';

        if (strncmp(out, "http://", 7) == 0 || strncmp(out, "https://", 8) == 0) {
            return out;
        }
        free(out);
        p = v;
    }
    return NULL;
}

static char *extract_first_url_token(const char *json)
{
    if (!json) {
        return NULL;
    }

    const char *p = strstr(json, "https://");
    const char *q = strstr(json, "http://");
    if (!p || (q && q < p)) {
        p = q;
    }
    if (!p) {
        return NULL;
    }

    char *out = (char *)malloc(YTMD_ART_URL_MAX_LEN);
    if (!out) {
        return NULL;
    }

    size_t n = 0;
    while (*p && *p != '"' && *p != '\\' && *p != ' ' && n + 1 < YTMD_ART_URL_MAX_LEN) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    if (n == 0) {
        free(out);
        return NULL;
    }
    return out;
}

static char *extract_art_url_from_song_json(const uint8_t *json_data, size_t json_len)
{
    if (!json_data || json_len == 0) {
        return NULL;
    }

    char *json = (char *)malloc(json_len + 1);
    if (!json) {
        return NULL;
    }
    memcpy(json, json_data, json_len);
    json[json_len] = '\0';

    char *url = extract_quoted_json_value_after_key(json, "\"cover\"");
    if (!url) {
        url = extract_quoted_json_value_after_key(json, "\"imageSrc\"");
    }
    if (!url) {
        url = extract_quoted_json_value_after_key(json, "\"url\"");
    }
    if (!url) {
        url = extract_first_url_token(json);
    }

    if (url) {
        normalize_art_url(url, YTMD_ART_URL_MAX_LEN);
    }

    free(json);
    return url;
}

static char *extract_video_id_from_song_json(const uint8_t *json_data, size_t json_len)
{
    if (!json_data || json_len == 0) {
        return NULL;
    }

    char *json = (char *)malloc(json_len + 1);
    if (!json) {
        return NULL;
    }
    memcpy(json, json_data, json_len);
    json[json_len] = '\0';

    char *video_id = extract_quoted_json_value_after_key(json, "\"videoId\"");
    if (video_id) {
        char *validated = dup_video_id_if_valid(video_id);
        free(video_id);
        if (validated) {
            free(json);
            return validated;
        }
    }

    video_id = extract_quoted_json_value_after_key(json, "\"id\"");
    if (video_id) {
        char *validated = dup_video_id_if_valid(video_id);
        free(video_id);
        if (validated) {
            free(json);
            return validated;
        }
    }

    const char *p = json;
    while ((p = strstr(p, "http")) != NULL) {
        char token[YTMD_ART_URL_MAX_LEN] = {0};
        size_t n = 0;
        while (p[n] && p[n] != '"' && p[n] != '\\' && p[n] != ' ' && n + 1 < sizeof(token)) {
            token[n] = p[n];
            n++;
        }
        token[n] = '\0';

        char vid[16] = {0};
        if (extract_video_id_from_watch_like_url(token, vid, sizeof(vid))) {
            free(json);
            return dup_cstr(vid);
        }
        p += (n > 0 ? n : 1);
    }

    free(json);
    return NULL;
}

static esp_err_t http_get_alloc(const char *url,
                                size_t max_size,
                                int timeout_ms,
                                bool text_mode,
                                ytmd_network_diag_cb_t net_diag_cb,
                                uint8_t **out_buf,
                                size_t *out_len)
{
    ESP_RETURN_ON_FALSE(url && out_buf && out_len, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "esp_http_client_init failed");
    if (!text_mode) {
        (void)esp_http_client_set_header(client, "Accept", "image/jpeg,image/*;q=0.9,*/*;q=0.8");
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed for %s: %s", url, esp_err_to_name(err));
        int tls_code = 0;
        int tls_flags = 0;
        if (esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags) == ESP_OK) {
            ESP_LOGW(TAG, "TLS detail: esp_tls_code=0x%x flags=0x%x", tls_code, tls_flags);
        }
        if (url && strncmp(url, "https://", 8) == 0 && net_diag_cb) {
            net_diag_cb(url);
        }
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *content_type = NULL;
    (void)esp_http_client_get_header(client, "Content-Type", &content_type);
    if (!text_mode) {
        ESP_LOGI(TAG, "HTTP image headers: status=%d len=%lld ct=%s",
                 status, (long long)content_len, content_type ? content_type : "(null)");
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP status=%d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t reserve = 4096;
    if (content_len > 0 && (size_t)content_len < max_size) {
        reserve = (size_t)content_len + (text_mode ? 1u : 0u);
    }
    if (reserve == 0) {
        reserve = 1;
    }

    uint8_t *buf = (uint8_t *)malloc(reserve);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    int read_len = 0;
    uint8_t tmp[1024];

    while ((read_len = esp_http_client_read(client, (char *)tmp, sizeof(tmp))) > 0) {
        size_t need = used + (size_t)read_len + (text_mode ? 1u : 0u);
        if (need > max_size) {
            ESP_LOGW(TAG, "HTTP body too large: %u > %u", (unsigned)need, (unsigned)max_size);
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
        if (need > reserve) {
            size_t new_cap = reserve;
            while (new_cap < need) {
                new_cap *= 2;
            }
            uint8_t *new_buf = (uint8_t *)realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            buf = new_buf;
            reserve = new_cap;
        }
        memcpy(buf + used, tmp, (size_t)read_len);
        used += (size_t)read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len < 0) {
        free(buf);
        return ESP_FAIL;
    }

    if (!text_mode && used >= 4) {
        ESP_LOGI(TAG, "HTTP image magic: %02X %02X %02X %02X (size=%u)",
                 buf[0], buf[1], buf[2], buf[3], (unsigned)used);
    }

    if (text_mode) {
        buf[used] = '\0';
    }

    *out_buf = buf;
    *out_len = used;
    return ESP_OK;
}

static inline int align_up_int(int value, int alignment)
{
    return ((value + alignment - 1) / alignment) * alignment;
}

static void get_jpeg_mcu_size(jpeg_down_sampling_type_t sample_method, int *mcux, int *mcuy)
{
    switch (sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444:
    case JPEG_DOWN_SAMPLING_GRAY:
        *mcux = 8;
        *mcuy = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        *mcux = 16;
        *mcuy = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV420:
    default:
        *mcux = 16;
        *mcuy = 16;
        break;
    }
}

static esp_err_t ensure_jpeg_engine(void)
{
    if (s_jpeg_decoder) {
        return ESP_OK;
    }
    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms = 120,
    };
    return jpeg_new_decoder_engine(&cfg, &s_jpeg_decoder);
}

static bool decode_jpeg_rgb565(const uint8_t *jpg,
                               size_t jpg_len,
                               uint16_t **out_pixels,
                               int *out_w,
                               int *out_h,
                               int *out_stride)
{
    if (!jpg || !out_pixels || !out_w || !out_h || !out_stride || jpg_len < 4) {
        return false;
    }

    *out_pixels = NULL;
    *out_w = 0;
    *out_h = 0;
    *out_stride = 0;

    if (ensure_jpeg_engine() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init JPEG decoder engine");
        return false;
    }

    jpeg_decode_picture_info_t info = {0};
    if (jpeg_decoder_get_info(jpg, (uint32_t)jpg_len, &info) != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_get_info failed");
        return false;
    }

    if (info.width == 0 || info.height == 0 || info.width > 4096 || info.height > 4096) {
        ESP_LOGW(TAG, "Invalid JPEG size: %ux%u", (unsigned)info.width, (unsigned)info.height);
        return false;
    }

    const bool grayscale = (info.sample_method == JPEG_DOWN_SAMPLING_GRAY);
    int mcu_x = 16;
    int mcu_y = 16;
    get_jpeg_mcu_size(info.sample_method, &mcu_x, &mcu_y);

    const int dec_stride = align_up_int((int)info.width, mcu_x);
    const int out_alloc_h = align_up_int((int)info.height, mcu_y);

    size_t bpp = grayscale ? 1u : 2u;
    size_t raw_size = (size_t)dec_stride * (size_t)out_alloc_h * bpp;

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t alloc_sz = 0;
    uint8_t *raw_out = (uint8_t *)jpeg_alloc_decoder_mem(raw_size, &mem_cfg, &alloc_sz);
    if (!raw_out) {
        ESP_LOGE(TAG, "jpeg_alloc_decoder_mem failed (%u bytes)", (unsigned)raw_size);
        return false;
    }

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = grayscale ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_jpeg_decoder,
                                         &dec_cfg,
                                         jpg,
                                         (uint32_t)jpg_len,
                                         raw_out,
                                         (uint32_t)alloc_sz,
                                         &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_process failed: %s", esp_err_to_name(err));
        free(raw_out);
        return false;
    }

    if (grayscale) {
        size_t rgb_size = (size_t)dec_stride * (size_t)out_alloc_h * 2u;
        uint16_t *rgb565 = (uint16_t *)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rgb565) {
            rgb565 = (uint16_t *)malloc(rgb_size);
        }
        if (!rgb565) {
            free(raw_out);
            return false;
        }
        for (int i = 0; i < dec_stride * out_alloc_h; i++) {
            uint8_t g = raw_out[i];
            rgb565[i] = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
        }
        free(raw_out);
        *out_pixels = rgb565;
    } else {
        *out_pixels = (uint16_t *)raw_out;
    }

    *out_w = (int)info.width;
    *out_h = (int)info.height;
    *out_stride = dec_stride;
    return true;
}

static void scale_crop_rgb565(const uint16_t *src,
                              int src_w,
                              int src_h,
                              int src_stride,
                              uint16_t *dst,
                              int dst_w,
                              int dst_h)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || src_stride < src_w || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = src_w;
    int crop_h = src_h;

    if ((int64_t)src_w * dst_h > (int64_t)src_h * dst_w) {
        crop_w = (int)((int64_t)src_h * dst_w / dst_h);
        crop_x = (src_w - crop_w) / 2;
    } else if ((int64_t)src_w * dst_h < (int64_t)src_h * dst_w) {
        crop_h = (int)((int64_t)src_w * dst_h / dst_w);
        crop_y = (src_h - crop_h) / 2;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = crop_y + (int)((int64_t)y * crop_h / dst_h);
        const uint16_t *src_row = src + (size_t)sy * src_stride;
        uint16_t *dst_row = dst + (size_t)y * dst_w;
        for (int x = 0; x < dst_w; x++) {
            int sx = crop_x + (int)((int64_t)x * crop_w / dst_w);
            dst_row[x] = src_row[sx];
        }
    }
}

esp_err_t ytmd_client_fetch_album_art(uint16_t *dst_rgb565,
                                      int dst_w,
                                      int dst_h,
                                      const char *last_art_url,
                                      char *out_art_url,
                                      size_t out_art_url_cap,
                                      ytmd_network_diag_cb_t net_diag_cb)
{
    ESP_RETURN_ON_FALSE(dst_rgb565 && dst_w > 0 && dst_h > 0, ESP_ERR_INVALID_ARG, TAG, "invalid destination");
    ESP_RETURN_ON_FALSE(out_art_url && out_art_url_cap > 0, ESP_ERR_INVALID_ARG, TAG, "invalid out_art_url");
    out_art_url[0] = '\0';

    uint8_t *json = NULL;
    size_t json_len = 0;
    esp_err_t err = http_get_alloc(
        YTMD_URL_API,
        MAX_JSON_BYTES,
        HTTP_TIMEOUT_MS,
        true,
        net_diag_cb,
        &json,
        &json_len);
    if (err != ESP_OK) {
        return err;
    }

    char *art_url = extract_art_url_from_song_json(json, json_len);
    char *video_id = extract_video_id_from_song_json(json, json_len);
    free(json);
    json = NULL;

    if ((!art_url || art_url[0] == '\0') && video_id) {
        char fb_url[YTMD_ART_URL_MAX_LEN] = {0};
        if (make_ytimg_fallback_url(video_id, fb_url, sizeof(fb_url))) {
            char *new_url = dup_cstr(fb_url);
            if (new_url) {
                free(art_url);
                art_url = new_url;
            }
        }
    }

    if (!art_url || art_url[0] == '\0') {
        free(art_url);
        free(video_id);
        return ESP_ERR_NOT_FOUND;
    }

    if (last_art_url && last_art_url[0] != '\0' && strncmp(art_url, last_art_url, YTMD_ART_URL_MAX_LEN - 1) == 0) {
        free(art_url);
        free(video_id);
        return ESP_ERR_NOT_FOUND;
    }

    char fb_url[YTMD_ART_URL_MAX_LEN] = {0};
    bool has_fallback = make_ytimg_fallback_url(video_id, fb_url, sizeof(fb_url));
    bool using_fallback_url = has_fallback && (strcmp(art_url, fb_url) == 0);
    ESP_LOGI(TAG, "Art URL selected=%s videoId=%s fallback=%s",
             art_url,
             video_id ? video_id : "(none)",
             has_fallback ? fb_url : "(none)");

    uint8_t *img = NULL;
    size_t img_len = 0;
    err = http_get_alloc(art_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, &img, &img_len);
    if (err != ESP_OK && has_fallback && !using_fallback_url) {
        ESP_LOGW(TAG, "Primary art URL failed, trying fallback: %s", fb_url);
        err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, &img, &img_len);
        if (err == ESP_OK) {
            char *new_url = dup_cstr(fb_url);
            if (new_url) {
                free(art_url);
                art_url = new_url;
                using_fallback_url = true;
            }
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Art download failed: %s", art_url);
        free(art_url);
        free(video_id);
        return err;
    }

    if (!is_jpeg_data(img, img_len)) {
        ESP_LOGW(TAG, "Art payload is not JPEG (url=%s)", art_url);
        if (has_fallback && !using_fallback_url) {
            ESP_LOGW(TAG, "Primary art is not JPEG, retry with fallback: %s", fb_url);
            free(img);
            img = NULL;
            img_len = 0;
            err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, &img, &img_len);
            if (err == ESP_OK) {
                char *new_url = dup_cstr(fb_url);
                if (new_url) {
                    free(art_url);
                    art_url = new_url;
                    using_fallback_url = true;
                }
            }
        }
        if (!img || !is_jpeg_data(img, img_len)) {
            ESP_LOGW(TAG, "No JPEG payload available after fallback");
            free(img);
            free(art_url);
            free(video_id);
            return ESP_FAIL;
        }
    }

    uint16_t *decoded = NULL;
    int w = 0;
    int h = 0;
    int stride = 0;
    bool ok = decode_jpeg_rgb565(img, img_len, &decoded, &w, &h, &stride);
    if (!ok && has_fallback && !using_fallback_url) {
        ESP_LOGW(TAG, "JPEG decode failed on primary URL, retry fallback: %s", fb_url);
        free(img);
        img = NULL;
        img_len = 0;
        err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, &img, &img_len);
        if (err == ESP_OK) {
            char *new_url = dup_cstr(fb_url);
            if (new_url) {
                free(art_url);
                art_url = new_url;
                using_fallback_url = true;
            }
            ok = decode_jpeg_rgb565(img, img_len, &decoded, &w, &h, &stride);
        }
    }
    free(img);
    img = NULL;

    if (!ok) {
        ESP_LOGW(TAG, "JPEG decode failed: %s", art_url);
        free(art_url);
        free(video_id);
        return ESP_FAIL;
    }

    scale_crop_rgb565(decoded, w, h, stride, dst_rgb565, dst_w, dst_h);
    free(decoded);

    snprintf(out_art_url, out_art_url_cap, "%s", art_url);
    free(art_url);
    free(video_id);

    ESP_LOGI(TAG, "Album art decoded: %dx%d", w, h);
    return ESP_OK;
}
