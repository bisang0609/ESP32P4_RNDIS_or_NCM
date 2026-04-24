#include "ytmd_client.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "driver/jpeg_decode.h"

static const char *TAG = "USB_NCM_YTMD_ART";

#define YTMD_API_BASE        "http://192.168.137.1:26538"
#define YTMD_URL_API_SONG    YTMD_API_BASE "/api/v1/song"
#define YTMD_URL_API_QUEUE   YTMD_API_BASE "/api/v1/queue"
#define YTMD_URL_API_QUEUE_NEXT YTMD_API_BASE "/api/v1/queue/next"
#define YTMD_URL_API_LIKE_STATE YTMD_API_BASE "/api/v1/like-state"
#define YTMD_URL_API_REPEAT_MODE YTMD_API_BASE "/api/v1/repeat-mode"
#define YTMD_URL_API_SHUFFLE YTMD_API_BASE "/api/v1/shuffle"
#define HTTP_TIMEOUT_MS      12000
#define HTTP_CMD_TIMEOUT_MS  3000
#define HTTP_STATE_TIMEOUT_MS 2000
#define HTTP_QUEUE_TIMEOUT_MS 2500
#define MAX_SONG_JSON_BYTES  (256 * 1024)
#define MAX_QUEUE_JSON_BYTES (3 * 1024 * 1024)
#define MAX_QUEUE_NEXT_JSON_BYTES (32 * 1024)
#define MAX_STATE_JSON_BYTES (8 * 1024)
#define MAX_IMAGE_BYTES      (3 * 1024 * 1024)
#define YTMD_QUEUE_CACHE_CAPACITY 1024
#define YTMD_ART_CACHE_CAPACITY 5
#define YTMD_ART_WINDOW_RADIUS 2
#define YTMD_PREFETCH_TASK_STACK 8192
#define YTMD_PREFETCH_TASK_PRIO 4

#define ART_SRC_REQ_W        400
#define ART_SRC_REQ_H        400
#define YTIMG_FALLBACK_FILE  "sddefault.jpg"

static jpeg_decoder_handle_t s_jpeg_decoder = NULL;
// Current backend does not expose /api/v1/queue/next reliably.
// Keep disabled by default and use /api/v1/queue parsing path.
static bool s_queue_next_supported = false;
static bool s_queue_fallback_enabled = true;
static ytmd_client_queue_item_t *s_queue_cache_items = NULL;
static size_t s_queue_cache_count = 0;
static int s_queue_cache_selected_pos = -1;
static SemaphoreHandle_t s_queue_cache_mutex = NULL;
static SemaphoreHandle_t s_art_cache_mutex = NULL;
static SemaphoreHandle_t s_prefetch_req_mutex = NULL;
static TaskHandle_t s_prefetch_task_handle = NULL;
static char s_last_logged_next_title[256] = {0};
static char s_last_logged_next_artist[128] = {0};
static char s_track_hint_title[256] = {0};
static char s_track_hint_artist[128] = {0};
static char s_track_hint_video_id[32] = {0};

typedef struct {
    bool valid;
    int width;
    int height;
    size_t bytes;
    uint32_t lru_tick;
    char key[YTMD_ART_URL_MAX_LEN];
    uint16_t *pixels;
} ytmd_art_cache_entry_t;

static ytmd_art_cache_entry_t s_art_cache[YTMD_ART_CACHE_CAPACITY] = {0};

typedef struct {
    bool valid;
    int dst_w;
    int dst_h;
    char current_cache_key[YTMD_ART_URL_MAX_LEN];
    ytmd_network_diag_cb_t net_diag_cb;
} ytmd_prefetch_request_t;

static ytmd_prefetch_request_t s_prefetch_request = {0};

static esp_err_t http_get_alloc(const char *url,
                                size_t max_size,
                                int timeout_ms,
                                bool text_mode,
                                ytmd_network_diag_cb_t net_diag_cb,
                                int *out_status,
                                bool *out_too_large,
                                uint8_t **out_buf,
                                size_t *out_len);
static bool extract_next_song_from_queue_json(const uint8_t *json_data,
                                              size_t json_len,
                                              char *out_title,
                                              size_t out_title_cap,
                                              char *out_artist,
                                              size_t out_artist_cap);
static bool extract_next_song_from_queue_next_json(const uint8_t *json_data,
                                                   size_t json_len,
                                                   char *out_title,
                                                   size_t out_title_cap,
                                                   char *out_artist,
                                                   size_t out_artist_cap);
static bool queue_cache_lock(TickType_t timeout_ticks);
static void queue_cache_unlock(void);
static bool ensure_queue_cache_alloc(void);
static int next_norm_alnum_char(const char **p);
static bool text_equal_norm_alnum(const char *a, const char *b);
static int queue_cache_find_selected_pos_by_hint_locked(const char *title,
                                                        const char *artist,
                                                        const char *video_id);
static bool queue_cache_copy_next_from_selected_locked(char *out_title,
                                                       size_t out_title_cap,
                                                       char *out_artist,
                                                       size_t out_artist_cap);
static void queue_cache_set_track_hint(const char *title,
                                       const char *artist,
                                       const char *video_id);
static bool art_cache_lock(TickType_t timeout_ticks);
static void art_cache_unlock(void);
static bool prefetch_req_lock(TickType_t timeout_ticks);
static void prefetch_req_unlock(void);
static bool fill_next_song_from_queue_cache(ytmd_client_playback_state_t *state);
static bool art_cache_copy_to_dst(const char *key, int dst_w, int dst_h, uint16_t *dst_rgb565);
static bool art_cache_store_from_src(const char *key, int src_w, int src_h, const uint16_t *src_rgb565);
static void prefetch_art_window_from_queue(const char *current_cache_key,
                                           int dst_w,
                                           int dst_h,
                                           ytmd_network_diag_cb_t net_diag_cb);
static void prefetch_worker_task(void *arg);
static void schedule_prefetch_art_window_from_queue(const char *current_cache_key,
                                                    int dst_w,
                                                    int dst_h,
                                                    ytmd_network_diag_cb_t net_diag_cb);
static bool decode_jpeg_rgb565(const uint8_t *jpg,
                               size_t jpg_len,
                               uint16_t **out_buf,
                               int *out_w,
                               int *out_h,
                               int *out_stride);
static void scale_crop_zoom_rgb565(const uint16_t *src,
                                   int src_w,
                                   int src_h,
                                   int src_stride,
                                   uint16_t *dst,
                                   int dst_w,
                                   int dst_h,
                                   int zoom_percent);

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

static bool starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) {
        return false;
    }
    while (*prefix) {
        if (*s++ != *prefix++) {
            return false;
        }
    }
    return true;
}

static void strip_url_query_and_fragment(char *url)
{
    if (!url) {
        return;
    }
    char *q = strchr(url, '?');
    char *h = strchr(url, '#');
    char *cut = NULL;
    if (q && h) {
        cut = (q < h) ? q : h;
    } else {
        cut = q ? q : h;
    }
    if (cut) {
        *cut = '\0';
    }
}

static bool is_googleusercontent_art_url(const char *url)
{
    if (!url) {
        return false;
    }
    return starts_with(url, "https://lh3.googleusercontent.com/") ||
           starts_with(url, "https://lh4.googleusercontent.com/") ||
           starts_with(url, "https://lh5.googleusercontent.com/") ||
           starts_with(url, "https://lh6.googleusercontent.com/");
}

static bool has_square_art_size_hint(const char *url)
{
    if (!url) {
        return false;
    }

    return strstr(url, "w400-h400") != NULL ||
           strstr(url, "w600-h600") != NULL ||
           strstr(url, "w800-h800") != NULL ||
           strstr(url, "=s400") != NULL ||
           strstr(url, "=s600") != NULL ||
           strstr(url, "=s800") != NULL;
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

static bool is_png_data(const uint8_t *buf, size_t len)
{
    if (!buf || len < 8) {
        return false;
    }
    return buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47 &&
           buf[4] == 0x0D && buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A;
}

static bool is_webp_data(const uint8_t *buf, size_t len)
{
    if (!buf || len < 12) {
        return false;
    }
    return memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WEBP", 4) == 0;
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

static void build_art_cache_key(const char *art_url, const char *video_id, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }
    out[0] = '\0';

    if (video_id && make_ytimg_fallback_url(video_id, out, out_cap)) {
        return;
    }

    if (!art_url || art_url[0] == '\0') {
        return;
    }

    snprintf(out, out_cap, "%s", art_url);
    strip_url_query_and_fragment(out);
    normalize_art_url(out, out_cap);
    strip_url_query_and_fragment(out);
}

static int hex_digit_to_int(char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (int)(c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (int)(c - 'A');
    }
    return -1;
}

static bool parse_hex4_u16(const char *s, uint16_t *out)
{
    if (!s || !out || s[0] == '\0' || s[1] == '\0' || s[2] == '\0' || s[3] == '\0') {
        return false;
    }

    int h0 = hex_digit_to_int(s[0]);
    int h1 = hex_digit_to_int(s[1]);
    int h2 = hex_digit_to_int(s[2]);
    int h3 = hex_digit_to_int(s[3]);
    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
        return false;
    }

    *out = (uint16_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
    return true;
}

static size_t append_utf8_codepoint(char *out, size_t out_cap, size_t n, uint32_t cp)
{
    if (!out || out_cap == 0 || n >= out_cap - 1) {
        return n;
    }

    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = '?';
    }

    if (cp <= 0x7F) {
        if (n + 1 < out_cap) {
            out[n++] = (char)cp;
        }
        return n;
    }

    if (cp <= 0x7FF) {
        if (n + 2 < out_cap) {
            out[n++] = (char)(0xC0 | ((cp >> 6) & 0x1F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
        return n;
    }

    if (cp <= 0xFFFF) {
        if (n + 3 < out_cap) {
            out[n++] = (char)(0xE0 | ((cp >> 12) & 0x0F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
        return n;
    }

    if (n + 4 < out_cap) {
        out[n++] = (char)(0xF0 | ((cp >> 18) & 0x07));
        out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    return n;
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

static char *extract_quoted_json_string_after_key(const char *json, const char *key_token, bool require_http_url)
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
                switch (c) {
                    case '"':
                    case '\\':
                    case '/':
                        if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                            out[n++] = c;
                        }
                        break;
                    case 'b':
                        if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                            out[n++] = '\b';
                        }
                        break;
                    case 'f':
                        if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                            out[n++] = '\f';
                        }
                        break;
                    case 'n':
                        if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                            out[n++] = '\n';
                        }
                        break;
                    case 'r':
                        if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                            out[n++] = '\r';
                        }
                        break;
                    case 't':
                        if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                            out[n++] = '\t';
                        }
                        break;
                    case 'u': {
                        uint16_t u0 = 0;
                        if (parse_hex4_u16(v, &u0)) {
                            v += 4;
                            uint32_t cp = u0;
                            if (u0 >= 0xD800 && u0 <= 0xDBFF && v[0] == '\\' && v[1] == 'u') {
                                uint16_t u1 = 0;
                                if (parse_hex4_u16(v + 2, &u1) && u1 >= 0xDC00 && u1 <= 0xDFFF) {
                                    cp = 0x10000 + (((uint32_t)(u0 - 0xD800) << 10) | (uint32_t)(u1 - 0xDC00));
                                    v += 6;
                                }
                            }
                            n = append_utf8_codepoint(out, YTMD_ART_URL_MAX_LEN, n, cp);
                        } else if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                            out[n++] = 'u';
                        }
                        break;
                    }
                    default:
                        if (n + 1 < YTMD_ART_URL_MAX_LEN) {
                            out[n++] = c;
                        }
                        break;
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

        if (!require_http_url || strncmp(out, "http://", 7) == 0 || strncmp(out, "https://", 8) == 0) {
            return out;
        }
        free(out);
        p = v;
    }
    return NULL;
}

static void extract_song_title_artist_from_json(const uint8_t *json_data,
                                                size_t json_len,
                                                char *out_title,
                                                size_t out_title_cap,
                                                char *out_artist,
                                                size_t out_artist_cap)
{
    if (out_title && out_title_cap > 0) {
        out_title[0] = '\0';
    }
    if (out_artist && out_artist_cap > 0) {
        out_artist[0] = '\0';
    }
    if (!json_data || json_len == 0) {
        return;
    }

    char *json = (char *)malloc(json_len + 1);
    if (!json) {
        return;
    }
    memcpy(json, json_data, json_len);
    json[json_len] = '\0';

    if (out_title && out_title_cap > 0) {
        static const char *title_keys[] = {
            "\"title\"",
            "\"name\"",
            "\"song\"",
        };
        for (size_t i = 0; i < sizeof(title_keys) / sizeof(title_keys[0]); ++i) {
            char *title = extract_quoted_json_string_after_key(json, title_keys[i], false);
            if (title && title[0] != '\0') {
                snprintf(out_title, out_title_cap, "%s", title);
                free(title);
                break;
            }
            free(title);
        }
    }

    if (out_artist && out_artist_cap > 0) {
        static const char *artist_keys[] = {
            "\"artist\"",
            "\"author\"",
            "\"artists\"",
            "\"subtitle\"",
        };
        for (size_t i = 0; i < sizeof(artist_keys) / sizeof(artist_keys[0]); ++i) {
            char *artist = extract_quoted_json_string_after_key(json, artist_keys[i], false);
            if (artist && artist[0] != '\0') {
                snprintf(out_artist, out_artist_cap, "%s", artist);
                free(artist);
                break;
            }
            free(artist);
        }
    }

    free(json);
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

    char *url = extract_quoted_json_string_after_key(json, "\"cover\"", true);
    if (!url) {
        url = extract_quoted_json_string_after_key(json, "\"imageSrc\"", true);
    }
    if (!url) {
        url = extract_quoted_json_string_after_key(json, "\"url\"", true);
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

    char *video_id = extract_quoted_json_string_after_key(json, "\"videoId\"", false);
    if (video_id) {
        char *validated = dup_video_id_if_valid(video_id);
        free(video_id);
        if (validated) {
            free(json);
            return validated;
        }
    }

    video_id = extract_quoted_json_string_after_key(json, "\"id\"", false);
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

static void init_playback_state(ytmd_client_playback_state_t *state)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->repeat = YTMD_CLIENT_REPEAT_NONE;
    state->seek_seconds = -1;
    state->elapsed_seconds = -1;
    state->song_duration_seconds = -1;
}

static void copy_string_field(char *dst, size_t dst_cap, const char *src)
{
    if (!dst || dst_cap == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src || src[0] == '\0') {
        return;
    }
    snprintf(dst, dst_cap, "%s", src);
}

static bool queue_cache_lock(TickType_t timeout_ticks)
{
    if (!s_queue_cache_mutex) {
        s_queue_cache_mutex = xSemaphoreCreateMutex();
        if (!s_queue_cache_mutex) {
            return false;
        }
    }
    return xSemaphoreTake(s_queue_cache_mutex, timeout_ticks) == pdTRUE;
}

static void queue_cache_unlock(void)
{
    if (s_queue_cache_mutex) {
        xSemaphoreGive(s_queue_cache_mutex);
    }
}

static bool art_cache_lock(TickType_t timeout_ticks)
{
    if (!s_art_cache_mutex) {
        s_art_cache_mutex = xSemaphoreCreateMutex();
        if (!s_art_cache_mutex) {
            return false;
        }
    }
    return xSemaphoreTake(s_art_cache_mutex, timeout_ticks) == pdTRUE;
}

static void art_cache_unlock(void)
{
    if (s_art_cache_mutex) {
        xSemaphoreGive(s_art_cache_mutex);
    }
}

static bool prefetch_req_lock(TickType_t timeout_ticks)
{
    if (!s_prefetch_req_mutex) {
        s_prefetch_req_mutex = xSemaphoreCreateMutex();
        if (!s_prefetch_req_mutex) {
            return false;
        }
    }
    return xSemaphoreTake(s_prefetch_req_mutex, timeout_ticks) == pdTRUE;
}

static void prefetch_req_unlock(void)
{
    if (s_prefetch_req_mutex) {
        xSemaphoreGive(s_prefetch_req_mutex);
    }
}

static bool ensure_queue_cache_alloc(void)
{
    if (s_queue_cache_items) {
        return true;
    }

    s_queue_cache_items = (ytmd_client_queue_item_t *)heap_caps_calloc(
        YTMD_QUEUE_CACHE_CAPACITY,
        sizeof(ytmd_client_queue_item_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue_cache_items) {
        s_queue_cache_items = (ytmd_client_queue_item_t *)calloc(
            YTMD_QUEUE_CACHE_CAPACITY, sizeof(ytmd_client_queue_item_t));
    }
    if (!s_queue_cache_items) {
        ESP_LOGW(TAG, "QUEUE CACHE: alloc failed (%u items)", (unsigned)YTMD_QUEUE_CACHE_CAPACITY);
        return false;
    }
    return true;
}

static int next_norm_alnum_char(const char **p)
{
    if (!p || !*p) {
        return 0;
    }
    while (**p) {
        unsigned char c = (unsigned char)**p;
        (*p)++;
        if (isalnum(c)) {
            return tolower(c);
        }
    }
    return 0;
}

static bool text_equal_norm_alnum(const char *a, const char *b)
{
    if (!a || !b || a[0] == '\0' || b[0] == '\0') {
        return false;
    }

    const char *pa = a;
    const char *pb = b;
    while (1) {
        int ca = next_norm_alnum_char(&pa);
        int cb = next_norm_alnum_char(&pb);
        if (ca != cb) {
            return false;
        }
        if (ca == 0) {
            return true;
        }
    }
}

static int queue_cache_find_selected_pos_by_hint_locked(const char *title,
                                                        const char *artist,
                                                        const char *video_id)
{
    if (!s_queue_cache_items || s_queue_cache_count == 0) {
        return -1;
    }

    int best_pos = -1;
    int best_score = 0;
    for (size_t i = 0; i < s_queue_cache_count; ++i) {
        const ytmd_client_queue_item_t *item = &s_queue_cache_items[i];
        int score = 0;

        if (video_id && video_id[0] != '\0' &&
            item->video_id[0] != '\0' &&
            strncmp(item->video_id, video_id, sizeof(item->video_id) - 1) == 0) {
            score += 100;
        }
        if (title && title[0] != '\0' &&
            item->title[0] != '\0' &&
            text_equal_norm_alnum(item->title, title)) {
            score += 35;
        }
        if (artist && artist[0] != '\0' &&
            item->artist[0] != '\0' &&
            text_equal_norm_alnum(item->artist, artist)) {
            score += 20;
        }

        if (score > best_score) {
            best_score = score;
            best_pos = (int)i;
        }
    }

    return (best_score > 0) ? best_pos : -1;
}

static bool queue_cache_copy_next_from_selected_locked(char *out_title,
                                                       size_t out_title_cap,
                                                       char *out_artist,
                                                       size_t out_artist_cap)
{
    if (!s_queue_cache_items || s_queue_cache_count == 0 || s_queue_cache_selected_pos < 0) {
        return false;
    }

    int next_idx = -1;
    int selected_row = s_queue_cache_items[s_queue_cache_selected_pos].index;
    if (selected_row >= 0) {
        int best_row = INT_MAX;
        for (size_t i = 0; i < s_queue_cache_count; ++i) {
            const ytmd_client_queue_item_t *entry = &s_queue_cache_items[i];
            if (entry->index > selected_row && entry->index < best_row) {
                if (entry->title[0] != '\0' || entry->artist[0] != '\0') {
                    best_row = entry->index;
                    next_idx = (int)i;
                }
            }
        }
    }

    if (next_idx < 0) {
        int fallback_idx = s_queue_cache_selected_pos + 1;
        if (fallback_idx >= 0 && (size_t)fallback_idx < s_queue_cache_count) {
            next_idx = fallback_idx;
        }
    }

    if (next_idx < 0 || (size_t)next_idx >= s_queue_cache_count) {
        return false;
    }

    const ytmd_client_queue_item_t *item = &s_queue_cache_items[next_idx];
    if (out_title && out_title_cap > 0) {
        copy_string_field(out_title, out_title_cap, item->title);
    }
    if (out_artist && out_artist_cap > 0) {
        copy_string_field(out_artist, out_artist_cap, item->artist);
    }

    return (item->title[0] != '\0' || item->artist[0] != '\0');
}

static void queue_cache_set_track_hint(const char *title,
                                       const char *artist,
                                       const char *video_id)
{
    copy_string_field(s_track_hint_title, sizeof(s_track_hint_title), title);
    copy_string_field(s_track_hint_artist, sizeof(s_track_hint_artist), artist);
    copy_string_field(s_track_hint_video_id, sizeof(s_track_hint_video_id), video_id);

    if (!queue_cache_lock(pdMS_TO_TICKS(50))) {
        return;
    }

    int pos = queue_cache_find_selected_pos_by_hint_locked(
        s_track_hint_title,
        s_track_hint_artist,
        s_track_hint_video_id);
    if (pos >= 0) {
        s_queue_cache_selected_pos = pos;
        for (size_t i = 0; i < s_queue_cache_count; ++i) {
            s_queue_cache_items[i].selected = ((int)i == pos);
        }
    }

    queue_cache_unlock();
}

static bool fill_next_song_from_queue_cache(ytmd_client_playback_state_t *state)
{
    if (!state || state->has_next_song) {
        return state && state->has_next_song;
    }

    if (!queue_cache_lock(pdMS_TO_TICKS(50))) {
        return false;
    }

    bool ok = false;
    if (s_queue_cache_selected_pos < 0) {
        int hint_pos = queue_cache_find_selected_pos_by_hint_locked(
            s_track_hint_title,
            s_track_hint_artist,
            s_track_hint_video_id);
        if (hint_pos >= 0) {
            s_queue_cache_selected_pos = hint_pos;
            for (size_t i = 0; i < s_queue_cache_count; ++i) {
                s_queue_cache_items[i].selected = ((int)i == hint_pos);
            }
        }
    }

    if (queue_cache_copy_next_from_selected_locked(state->next_title,
                                                   sizeof(state->next_title),
                                                   state->next_artist,
                                                   sizeof(state->next_artist))) {
        state->has_next_song = true;
        ok = true;
    }

    queue_cache_unlock();
    return ok;
}

static void log_next_track_if_changed(const char *source, const char *title, const char *artist)
{
    const char *t = title ? title : "";
    const char *a = artist ? artist : "";
    if (strncmp(s_last_logged_next_title, t, sizeof(s_last_logged_next_title) - 1) == 0 &&
        strncmp(s_last_logged_next_artist, a, sizeof(s_last_logged_next_artist) - 1) == 0) {
        return;
    }

    copy_string_field(s_last_logged_next_title, sizeof(s_last_logged_next_title), t);
    copy_string_field(s_last_logged_next_artist, sizeof(s_last_logged_next_artist), a);

    ESP_LOGI(TAG, "NEXT: parsed from %s => '%s - %s'",
             source ? source : "queue",
             t[0] ? t : "-",
             a[0] ? a : "-");
}

static uint32_t art_cache_now_tick(void)
{
    return (uint32_t)xTaskGetTickCount();
}

static bool art_cache_ensure_entry_buffer(ytmd_art_cache_entry_t *entry, size_t bytes)
{
    if (!entry || bytes == 0) {
        return false;
    }

    if (entry->pixels && entry->bytes == bytes) {
        return true;
    }

    free(entry->pixels);
    entry->pixels = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entry->pixels) {
        entry->pixels = (uint16_t *)malloc(bytes);
    }
    if (!entry->pixels) {
        entry->bytes = 0;
        return false;
    }

    entry->bytes = bytes;
    return true;
}

static int art_cache_find_index(const char *key, int w, int h)
{
    if (!key || key[0] == '\0' || w <= 0 || h <= 0) {
        return -1;
    }

    for (int i = 0; i < YTMD_ART_CACHE_CAPACITY; ++i) {
        ytmd_art_cache_entry_t *entry = &s_art_cache[i];
        if (!entry->valid || !entry->pixels) {
            continue;
        }
        if (entry->width == w &&
            entry->height == h &&
            strncmp(entry->key, key, sizeof(entry->key) - 1) == 0) {
            return i;
        }
    }
    return -1;
}

static int art_cache_pick_victim_index(void)
{
    for (int i = 0; i < YTMD_ART_CACHE_CAPACITY; ++i) {
        if (!s_art_cache[i].valid) {
            return i;
        }
    }

    int victim = 0;
    uint32_t oldest = s_art_cache[0].lru_tick;
    for (int i = 1; i < YTMD_ART_CACHE_CAPACITY; ++i) {
        if (s_art_cache[i].lru_tick < oldest) {
            oldest = s_art_cache[i].lru_tick;
            victim = i;
        }
    }
    return victim;
}

static bool art_cache_has_key(const char *key, int w, int h)
{
    if (!art_cache_lock(pdMS_TO_TICKS(50))) {
        return false;
    }
    bool hit = art_cache_find_index(key, w, h) >= 0;
    art_cache_unlock();
    return hit;
}

static bool art_cache_copy_to_dst(const char *key, int dst_w, int dst_h, uint16_t *dst_rgb565)
{
    if (!key || !dst_rgb565 || dst_w <= 0 || dst_h <= 0) {
        return false;
    }

    if (!art_cache_lock(pdMS_TO_TICKS(50))) {
        return false;
    }

    int idx = art_cache_find_index(key, dst_w, dst_h);
    if (idx < 0) {
        art_cache_unlock();
        return false;
    }

    ytmd_art_cache_entry_t *entry = &s_art_cache[idx];
    size_t bytes = (size_t)dst_w * (size_t)dst_h * 2u;
    if (!entry->pixels || entry->bytes < bytes) {
        art_cache_unlock();
        return false;
    }

    memcpy(dst_rgb565, entry->pixels, bytes);
    entry->lru_tick = art_cache_now_tick();
    art_cache_unlock();
    ESP_LOGD(TAG, "ART CACHE: hit key=%s", key);
    return true;
}

static bool art_cache_store_from_src(const char *key, int src_w, int src_h, const uint16_t *src_rgb565)
{
    if (!key || key[0] == '\0' || !src_rgb565 || src_w <= 0 || src_h <= 0) {
        return false;
    }

    if (!art_cache_lock(pdMS_TO_TICKS(50))) {
        return false;
    }

    size_t bytes = (size_t)src_w * (size_t)src_h * 2u;
    int idx = art_cache_find_index(key, src_w, src_h);
    if (idx < 0) {
        idx = art_cache_pick_victim_index();
    }

    ytmd_art_cache_entry_t *entry = &s_art_cache[idx];
    if (!art_cache_ensure_entry_buffer(entry, bytes)) {
        art_cache_unlock();
        return false;
    }

    memcpy(entry->pixels, src_rgb565, bytes);
    entry->valid = true;
    entry->width = src_w;
    entry->height = src_h;
    entry->lru_tick = art_cache_now_tick();
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    art_cache_unlock();
    return true;
}

static esp_err_t download_decode_art_to_rgb565(const char *art_url,
                                               int dst_w,
                                               int dst_h,
                                               uint16_t *dst_rgb565,
                                               ytmd_network_diag_cb_t net_diag_cb)
{
    if (!art_url || art_url[0] == '\0' || dst_w <= 0 || dst_h <= 0 || !dst_rgb565) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *img = NULL;
    size_t img_len = 0;
    esp_err_t err = http_get_alloc(art_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, NULL, NULL, &img, &img_len);
    if (err != ESP_OK) {
        return err;
    }

    if (!is_jpeg_data(img, img_len)) {
        free(img);
        return ESP_FAIL;
    }

    uint16_t *decoded = NULL;
    int w = 0;
    int h = 0;
    int stride = 0;
    bool ok = decode_jpeg_rgb565(img, img_len, &decoded, &w, &h, &stride);
    free(img);
    if (!ok || !decoded) {
        free(decoded);
        return ESP_FAIL;
    }

    int crop_zoom_percent = 100;
    if (dst_w == dst_h && w > 0 && h > 0) {
        int max_dim = (w > h) ? w : h;
        int min_dim = (w > h) ? h : w;
        // Non-square thumbnails (e.g., 16:9) look too small in square art slots.
        // Match main fetch-path behavior by adding center zoom.
        if ((int64_t)max_dim * 10 >= (int64_t)min_dim * 12) {
            crop_zoom_percent = 145;
        }
    }

    scale_crop_zoom_rgb565(decoded, w, h, stride, dst_rgb565, dst_w, dst_h, crop_zoom_percent);
    free(decoded);
    return ESP_OK;
}

static void prefetch_art_window_from_queue(const char *current_cache_key,
                                           int dst_w,
                                           int dst_h,
                                           ytmd_network_diag_cb_t net_diag_cb)
{
    if (dst_w <= 0 || dst_h <= 0) {
        return;
    }

    uint8_t *queue_json = NULL;
    size_t queue_json_len = 0;
    bool queue_too_large = false;
    esp_err_t err = http_get_alloc(YTMD_URL_API_QUEUE,
                                   MAX_QUEUE_JSON_BYTES,
                                   HTTP_QUEUE_TIMEOUT_MS,
                                   true,
                                   net_diag_cb,
                                   NULL,
                                   &queue_too_large,
                                   &queue_json,
                                   &queue_json_len);
    if (err != ESP_OK || !queue_json || queue_json_len == 0) {
        free(queue_json);
        if (queue_too_large) {
            ESP_LOGW(TAG, "ART CACHE: skip prefetch, queue payload exceeded %u bytes", (unsigned)MAX_QUEUE_JSON_BYTES);
        }
        return;
    }

    char dummy_title[4] = {0};
    char dummy_artist[4] = {0};
    (void)extract_next_song_from_queue_json(queue_json, queue_json_len, dummy_title, sizeof(dummy_title), dummy_artist, sizeof(dummy_artist));
    free(queue_json);

    typedef struct {
        char key[YTMD_ART_URL_MAX_LEN];
        char url[YTMD_ART_URL_MAX_LEN];
    } prefetch_item_t;

    prefetch_item_t *targets = (prefetch_item_t *)heap_caps_calloc(
        YTMD_ART_CACHE_CAPACITY, sizeof(prefetch_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!targets) {
        targets = (prefetch_item_t *)calloc(YTMD_ART_CACHE_CAPACITY, sizeof(prefetch_item_t));
    }
    if (!targets) {
        ESP_LOGW(TAG, "ART CACHE: prefetch target alloc failed");
        return;
    }

    size_t target_count = 0;
    bool cache_locked = false;

    if (!queue_cache_lock(pdMS_TO_TICKS(50))) {
        free(targets);
        return;
    }
    cache_locked = true;

    if (!s_queue_cache_items || s_queue_cache_count == 0 || s_queue_cache_selected_pos < 0) {
        if (cache_locked) {
            queue_cache_unlock();
        }
        free(targets);
        return;
    }

    for (int rel = -YTMD_ART_WINDOW_RADIUS; rel <= YTMD_ART_WINDOW_RADIUS; ++rel) {
        int idx = s_queue_cache_selected_pos + rel;
        if (idx < 0 || (size_t)idx >= s_queue_cache_count) {
            continue;
        }

        const ytmd_client_queue_item_t *item = &s_queue_cache_items[idx];
        char prefetch_url[YTMD_ART_URL_MAX_LEN] = {0};
        char prefetch_key[YTMD_ART_URL_MAX_LEN] = {0};

        if (item->video_id[0] != '\0') {
            if (!make_ytimg_fallback_url(item->video_id, prefetch_url, sizeof(prefetch_url))) {
                continue;
            }
            build_art_cache_key(prefetch_url, item->video_id, prefetch_key, sizeof(prefetch_key));
        } else if (item->art_url[0] != '\0') {
            snprintf(prefetch_url, sizeof(prefetch_url), "%s", item->art_url);
            build_art_cache_key(prefetch_url, NULL, prefetch_key, sizeof(prefetch_key));
        } else {
            continue;
        }

        if (prefetch_key[0] == '\0' || prefetch_url[0] == '\0') {
            continue;
        }
        if (current_cache_key && strncmp(current_cache_key, prefetch_key, sizeof(prefetch_key) - 1) == 0) {
            continue;
        }

        bool duplicate = false;
        for (size_t i = 0; i < target_count; ++i) {
            if (strncmp(targets[i].key, prefetch_key, sizeof(targets[i].key) - 1) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || target_count >= YTMD_ART_CACHE_CAPACITY) {
            continue;
        }

        snprintf(targets[target_count].key, sizeof(targets[target_count].key), "%s", prefetch_key);
        snprintf(targets[target_count].url, sizeof(targets[target_count].url), "%s", prefetch_url);
        target_count++;
    }

    if (cache_locked) {
        queue_cache_unlock();
    }

    if (target_count == 0) {
        free(targets);
        return;
    }

    size_t frame_bytes = (size_t)dst_w * (size_t)dst_h * 2u;
    uint16_t *tmp = (uint16_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        tmp = (uint16_t *)malloc(frame_bytes);
    }
    if (!tmp) {
        free(targets);
        return;
    }

    for (size_t i = 0; i < target_count; ++i) {
        if (art_cache_has_key(targets[i].key, dst_w, dst_h)) {
            continue;
        }

        esp_err_t pf_err = download_decode_art_to_rgb565(targets[i].url, dst_w, dst_h, tmp, net_diag_cb);
        if (pf_err == ESP_OK) {
            (void)art_cache_store_from_src(targets[i].key, dst_w, dst_h, tmp);
        }
    }

    free(tmp);
    free(targets);
}

static void prefetch_worker_task(void *arg)
{
    (void)arg;

    while (1) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ytmd_prefetch_request_t req = {0};
        bool has_req = false;
        if (prefetch_req_lock(pdMS_TO_TICKS(50))) {
            has_req = s_prefetch_request.valid;
            if (has_req) {
                req = s_prefetch_request;
                s_prefetch_request.valid = false;
            }
            prefetch_req_unlock();
        }

        if (!has_req) {
            continue;
        }

        prefetch_art_window_from_queue(
            req.current_cache_key[0] ? req.current_cache_key : NULL,
            req.dst_w,
            req.dst_h,
            req.net_diag_cb);
    }
}

static void schedule_prefetch_art_window_from_queue(const char *current_cache_key,
                                                    int dst_w,
                                                    int dst_h,
                                                    ytmd_network_diag_cb_t net_diag_cb)
{
    if (dst_w <= 0 || dst_h <= 0) {
        return;
    }

    if (!s_prefetch_task_handle) {
        BaseType_t ok = xTaskCreate(
            prefetch_worker_task,
            "ytmd_prefetch",
            YTMD_PREFETCH_TASK_STACK,
            NULL,
            YTMD_PREFETCH_TASK_PRIO,
            &s_prefetch_task_handle);
        if (ok != pdPASS) {
            s_prefetch_task_handle = NULL;
            ESP_LOGW(TAG, "ART CACHE: failed to create prefetch task");
            return;
        }
    }

    if (!prefetch_req_lock(pdMS_TO_TICKS(50))) {
        return;
    }

    memset(&s_prefetch_request, 0, sizeof(s_prefetch_request));
    s_prefetch_request.valid = true;
    s_prefetch_request.dst_w = dst_w;
    s_prefetch_request.dst_h = dst_h;
    s_prefetch_request.net_diag_cb = net_diag_cb;
    if (current_cache_key && current_cache_key[0] != '\0') {
        snprintf(s_prefetch_request.current_cache_key,
                 sizeof(s_prefetch_request.current_cache_key),
                 "%s",
                 current_cache_key);
    }

    prefetch_req_unlock();
    xTaskNotifyGive(s_prefetch_task_handle);
}

static bool extract_json_bool_after_key(const char *json, const char *key_token, bool *out_value)
{
    if (!json || !key_token || !out_value) {
        return false;
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

        if (strncmp(v, "true", 4) == 0) {
            *out_value = true;
            return true;
        }
        if (strncmp(v, "false", 5) == 0) {
            *out_value = false;
            return true;
        }
        if (*v == '1') {
            *out_value = true;
            return true;
        }
        if (*v == '0') {
            *out_value = false;
            return true;
        }
        if (*v == '"') {
            v++;
            if (strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || strncmp(v, "1", 1) == 0) {
                *out_value = true;
                return true;
            }
            if (strncmp(v, "false", 5) == 0 || strncmp(v, "off", 3) == 0 || strncmp(v, "0", 1) == 0) {
                *out_value = false;
                return true;
            }
        }

        p = colon + 1;
    }

    return false;
}

static bool extract_json_int_after_key(const char *json, const char *key_token, int *out_value)
{
    if (!json || !key_token || !out_value) {
        return false;
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

        char *end_ptr = NULL;
        long value = strtol(v, &end_ptr, 10);
        if (end_ptr != v) {
            *out_value = (int)value;
            return true;
        }

        p = colon + 1;
    }

    return false;
}

static const char *skip_json_ws(const char *s)
{
    if (!s) {
        return NULL;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    return s;
}

static bool extract_top_level_token(const char *json, char *out, size_t out_cap)
{
    if (!json || !out || out_cap == 0) {
        return false;
    }

    out[0] = '\0';
    const char *p = skip_json_ws(json);
    if (!p || p[0] == '\0') {
        return false;
    }

    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < out_cap) {
            out[n++] = (char)tolower((unsigned char)*p++);
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
               n + 1 < out_cap) {
            out[n++] = (char)tolower((unsigned char)*p++);
        }
    }
    out[n] = '\0';
    return n > 0;
}

static bool parse_bool_token(const char *token, bool *out_value)
{
    if (!token || !out_value) {
        return false;
    }

    if (strcmp(token, "1") == 0 || strcmp(token, "true") == 0 ||
        strcmp(token, "on") == 0 || strstr(token, "enable") != NULL ||
        strcmp(token, "active") == 0) {
        *out_value = true;
        return true;
    }
    if (strcmp(token, "0") == 0 || strcmp(token, "false") == 0 ||
        strcmp(token, "off") == 0 || strstr(token, "disable") != NULL ||
        strcmp(token, "inactive") == 0 || strcmp(token, "none") == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool parse_top_level_bool(const char *json, bool *out_value)
{
    char token[32] = {0};
    if (!extract_top_level_token(json, token, sizeof(token))) {
        return false;
    }
    return parse_bool_token(token, out_value);
}

static bool parse_top_level_int(const char *json, int *out_value)
{
    if (!json || !out_value) {
        return false;
    }
    const char *p = skip_json_ws(json);
    if (!p || p[0] == '\0') {
        return false;
    }
    char *end_ptr = NULL;
    long v = strtol(p, &end_ptr, 10);
    if (end_ptr == p) {
        return false;
    }
    *out_value = (int)v;
    return true;
}

static void parse_repeat_mode(const char *repeat_str, ytmd_client_repeat_t *out_repeat)
{
    if (!repeat_str || !out_repeat) {
        return;
    }

    char normalized[24] = {0};
    size_t n = strlen(repeat_str);
    if (n >= sizeof(normalized)) {
        n = sizeof(normalized) - 1;
    }
    for (size_t i = 0; i < n; i++) {
        normalized[i] = (char)tolower((unsigned char)repeat_str[i]);
    }

    if (strstr(normalized, "all")) {
        *out_repeat = YTMD_CLIENT_REPEAT_ALL;
    } else if (strstr(normalized, "one") || strstr(normalized, "single")) {
        *out_repeat = YTMD_CLIENT_REPEAT_ONE;
    } else {
        *out_repeat = YTMD_CLIENT_REPEAT_NONE;
    }
}

static void map_repeat_int_mode(int repeat_int, ytmd_client_repeat_t *out_repeat)
{
    if (!out_repeat) {
        return;
    }
    if (repeat_int == 1) {
        *out_repeat = YTMD_CLIENT_REPEAT_ALL;
    } else if (repeat_int == 2) {
        *out_repeat = YTMD_CLIENT_REPEAT_ONE;
    } else {
        *out_repeat = YTMD_CLIENT_REPEAT_NONE;
    }
}

static bool parse_shuffle_state_from_json(const char *json, bool *out_shuffle)
{
    if (!json || !out_shuffle) {
        return false;
    }

    bool v = false;
    if (extract_json_bool_after_key(json, "\"shuffleEnabled\"", &v) ||
        extract_json_bool_after_key(json, "\"shuffleMode\"", &v) ||
        extract_json_bool_after_key(json, "\"isShuffle\"", &v) ||
        extract_json_bool_after_key(json, "\"shuffle\"", &v) ||
        extract_json_bool_after_key(json, "\"enabled\"", &v) ||
        parse_top_level_bool(json, &v)) {
        *out_shuffle = v;
        return true;
    }

    int mode = 0;
    if (extract_json_int_after_key(json, "\"shuffleMode\"", &mode) ||
        extract_json_int_after_key(json, "\"shuffle\"", &mode) ||
        extract_json_int_after_key(json, "\"enabled\"", &mode) ||
        parse_top_level_int(json, &mode)) {
        *out_shuffle = (mode != 0);
        return true;
    }

    char *shuffle_state = extract_quoted_json_string_after_key(json, "\"shuffleMode\"", false);
    if (!shuffle_state) {
        shuffle_state = extract_quoted_json_string_after_key(json, "\"shuffle\"", false);
    }
    if (!shuffle_state) {
        shuffle_state = extract_quoted_json_string_after_key(json, "\"state\"", false);
    }
    if (!shuffle_state) {
        shuffle_state = extract_quoted_json_string_after_key(json, "\"value\"", false);
    }

    if (shuffle_state) {
        char lowered[32] = {0};
        size_t n = strlen(shuffle_state);
        if (n >= sizeof(lowered)) {
            n = sizeof(lowered) - 1;
        }
        for (size_t i = 0; i < n; i++) {
            lowered[i] = (char)tolower((unsigned char)shuffle_state[i]);
        }
        bool parsed = parse_bool_token(lowered, out_shuffle);
        free(shuffle_state);
        return parsed;
    }

    return false;
}

static bool parse_repeat_state_from_json(const char *json, ytmd_client_repeat_t *out_repeat)
{
    if (!json || !out_repeat) {
        return false;
    }

    char *repeat = extract_quoted_json_string_after_key(json, "\"repeatType\"", false);
    if (!repeat) {
        repeat = extract_quoted_json_string_after_key(json, "\"repeatMode\"", false);
    }
    if (!repeat) {
        repeat = extract_quoted_json_string_after_key(json, "\"mode\"", false);
    }
    if (!repeat) {
        repeat = extract_quoted_json_string_after_key(json, "\"state\"", false);
    }
    if (!repeat) {
        repeat = extract_quoted_json_string_after_key(json, "\"value\"", false);
    }

    if (repeat) {
        parse_repeat_mode(repeat, out_repeat);
        free(repeat);
        return true;
    }

    int repeat_int = 0;
    if (extract_json_int_after_key(json, "\"repeatType\"", &repeat_int) ||
        extract_json_int_after_key(json, "\"repeatMode\"", &repeat_int) ||
        extract_json_int_after_key(json, "\"mode\"", &repeat_int) ||
        extract_json_int_after_key(json, "\"state\"", &repeat_int) ||
        extract_json_int_after_key(json, "\"value\"", &repeat_int) ||
        parse_top_level_int(json, &repeat_int)) {
        map_repeat_int_mode(repeat_int, out_repeat);
        return true;
    }

    char top_token[24] = {0};
    if (extract_top_level_token(json, top_token, sizeof(top_token))) {
        parse_repeat_mode(top_token, out_repeat);
        return true;
    }

    return false;
}

static void apply_like_status_token(const char *like_status, ytmd_client_playback_state_t *state)
{
    if (!like_status || !state) {
        return;
    }

    char lowered[32] = {0};
    size_t n = strlen(like_status);
    if (n >= sizeof(lowered)) {
        n = sizeof(lowered) - 1;
    }
    for (size_t i = 0; i < n; i++) {
        lowered[i] = (char)tolower((unsigned char)like_status[i]);
    }

    if (strstr(lowered, "dislike") || strstr(lowered, "thumb_down")) {
        state->has_liked = true;
        state->is_liked = false;
        state->has_disliked = true;
        state->is_disliked = true;
    } else if (strstr(lowered, "like") || strstr(lowered, "thumb_up")) {
        state->has_liked = true;
        state->is_liked = true;
        state->has_disliked = true;
        state->is_disliked = false;
    } else if (strstr(lowered, "none") || strstr(lowered, "neutral")) {
        state->has_liked = true;
        state->is_liked = false;
        state->has_disliked = true;
        state->is_disliked = false;
    }
}

static bool parse_like_state_from_json(const char *json, ytmd_client_playback_state_t *state)
{
    if (!json || !state) {
        return false;
    }

    bool parsed = false;
    bool v = false;
    if (extract_json_bool_after_key(json, "\"isLiked\"", &v) ||
        extract_json_bool_after_key(json, "\"liked\"", &v)) {
        state->has_liked = true;
        state->is_liked = v;
        parsed = true;
    }
    if (extract_json_bool_after_key(json, "\"isDisliked\"", &v) ||
        extract_json_bool_after_key(json, "\"disliked\"", &v)) {
        state->has_disliked = true;
        state->is_disliked = v;
        parsed = true;
    }

    char *like_status = extract_quoted_json_string_after_key(json, "\"likeStatus\"", false);
    if (!like_status) {
        like_status = extract_quoted_json_string_after_key(json, "\"state\"", false);
    }
    if (!like_status) {
        like_status = extract_quoted_json_string_after_key(json, "\"value\"", false);
    }
    if (like_status) {
        apply_like_status_token(like_status, state);
        free(like_status);
        parsed = true;
    }

    if (!parsed) {
        int state_int = 0;
        if (parse_top_level_int(json, &state_int)) {
            state->has_liked = true;
            state->has_disliked = true;
            state->is_liked = (state_int > 0);
            state->is_disliked = (state_int < 0);
            parsed = true;
        } else if (parse_top_level_bool(json, &v)) {
            state->has_liked = true;
            state->has_disliked = true;
            state->is_liked = v;
            state->is_disliked = false;
            parsed = true;
        } else {
            char token[32] = {0};
            if (extract_top_level_token(json, token, sizeof(token))) {
                apply_like_status_token(token, state);
                parsed = true;
            }
        }
    }

    return parsed;
}

static bool parse_seek_seconds_from_token(const char *token, int *out_seconds)
{
    if (!token || !out_seconds) {
        return false;
    }

    char buf[64] = {0};
    size_t n = 0;
    while (token[n] && token[n] != '/' && n + 1 < sizeof(buf)) {
        buf[n] = token[n];
        n++;
    }
    buf[n] = '\0';

    char *start = buf;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (*start == '\0') {
        return false;
    }

    char *end_trim = start + strlen(start);
    while (end_trim > start && isspace((unsigned char)end_trim[-1])) {
        end_trim--;
    }
    *end_trim = '\0';
    if (*start == '\0') {
        return false;
    }

    if (strchr(start, ':') == NULL) {
        char *end_ptr = NULL;
        long v = strtol(start, &end_ptr, 10);
        if (end_ptr == start || v < 0 || v > INT_MAX) {
            return false;
        }
        *out_seconds = (int)v;
        return true;
    }

    int parts[3] = {0};
    int part_count = 0;
    char *cursor = start;
    while (*cursor && part_count < 3) {
        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }

        char *part_end = NULL;
        long v = strtol(cursor, &part_end, 10);
        if (part_end == cursor || v < 0 || v > INT_MAX) {
            return false;
        }
        parts[part_count++] = (int)v;
        cursor = part_end;

        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == ':') {
            cursor++;
            continue;
        }
        break;
    }

    while (*cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return false;
    }
    if (part_count < 2 || part_count > 3) {
        return false;
    }

    int64_t sec = 0;
    if (part_count == 2) {
        sec = (int64_t)parts[0] * 60 + parts[1];
    } else {
        sec = (int64_t)parts[0] * 3600 + (int64_t)parts[1] * 60 + parts[2];
    }
    if (sec < 0 || sec > INT_MAX) {
        return false;
    }

    *out_seconds = (int)sec;
    return true;
}

static bool extract_seek_seconds_from_song_json(const char *json, int *out_seconds)
{
    if (!json || !out_seconds) {
        return false;
    }

    static const char *sec_keys[] = {
        "\"elapsedSeconds\"",
        "\"elapsedSec\"",
        "\"currentTimeSeconds\"",
        "\"currentSeconds\"",
        "\"positionSeconds\"",
        "\"seekSeconds\"",
    };
    for (size_t i = 0; i < sizeof(sec_keys) / sizeof(sec_keys[0]); ++i) {
        int v = 0;
        if (extract_json_int_after_key(json, sec_keys[i], &v) && v >= 0) {
            *out_seconds = v;
            return true;
        }
    }

    static const char *ms_keys[] = {
        "\"elapsedMilliseconds\"",
        "\"elapsedMs\"",
        "\"currentTimeMs\"",
        "\"positionMs\"",
        "\"seekMs\"",
    };
    for (size_t i = 0; i < sizeof(ms_keys) / sizeof(ms_keys[0]); ++i) {
        int v = 0;
        if (extract_json_int_after_key(json, ms_keys[i], &v) && v >= 0) {
            *out_seconds = v / 1000;
            return true;
        }
    }

    static const char *text_keys[] = {
        "\"elapsed\"",
        "\"currentTime\"",
        "\"position\"",
        "\"seek\"",
    };
    for (size_t i = 0; i < sizeof(text_keys) / sizeof(text_keys[0]); ++i) {
        char *token = extract_quoted_json_string_after_key(json, text_keys[i], false);
        if (!token) {
            continue;
        }
        int sec = 0;
        bool ok = parse_seek_seconds_from_token(token, &sec);
        free(token);
        if (ok) {
            *out_seconds = sec;
            return true;
        }
    }

    return false;
}

static bool extract_song_duration_seconds_from_song_json(const char *json, int *out_seconds)
{
    if (!json || !out_seconds) {
        return false;
    }

    static const char *sec_keys[] = {
        "\"songDuration\"",
        "\"durationSeconds\"",
        "\"songDurationSeconds\"",
    };
    for (size_t i = 0; i < sizeof(sec_keys) / sizeof(sec_keys[0]); ++i) {
        int v = 0;
        if (extract_json_int_after_key(json, sec_keys[i], &v) && v >= 0) {
            *out_seconds = v;
            return true;
        }
    }

    static const char *ms_keys[] = {
        "\"songDurationMs\"",
        "\"durationMs\"",
    };
    for (size_t i = 0; i < sizeof(ms_keys) / sizeof(ms_keys[0]); ++i) {
        int v = 0;
        if (extract_json_int_after_key(json, ms_keys[i], &v) && v >= 0) {
            *out_seconds = v / 1000;
            return true;
        }
    }

    static const char *text_keys[] = {
        "\"songDuration\"",
        "\"duration\"",
    };
    for (size_t i = 0; i < sizeof(text_keys) / sizeof(text_keys[0]); ++i) {
        char *token = extract_quoted_json_string_after_key(json, text_keys[i], false);
        if (!token) {
            continue;
        }
        int sec = 0;
        bool ok = parse_seek_seconds_from_token(token, &sec);
        free(token);
        if (ok) {
            *out_seconds = sec;
            return true;
        }
    }

    return false;
}

static void extract_next_song_from_song_json(const char *json, ytmd_client_playback_state_t *state)
{
    if (!json || !state) {
        return;
    }

    char *next_title = extract_quoted_json_string_after_key(json, "\"nextTitle\"", false);
    if (!next_title) {
        next_title = extract_quoted_json_string_after_key(json, "\"nextSongTitle\"", false);
    }
    if (!next_title) {
        next_title = extract_quoted_json_string_after_key(json, "\"next_track_title\"", false);
    }

    char *next_artist = extract_quoted_json_string_after_key(json, "\"nextArtist\"", false);
    if (!next_artist) {
        next_artist = extract_quoted_json_string_after_key(json, "\"nextSongArtist\"", false);
    }
    if (!next_artist) {
        next_artist = extract_quoted_json_string_after_key(json, "\"next_track_artist\"", false);
    }

    if (!next_title || !next_artist) {
        const char *next_obj = strstr(json, "\"nextSong\"");
        if (next_obj) {
            if (!next_title) {
                next_title = extract_quoted_json_string_after_key(next_obj, "\"title\"", false);
            }
            if (!next_artist) {
                next_artist = extract_quoted_json_string_after_key(next_obj, "\"artist\"", false);
            }
            if (!next_artist) {
                next_artist = extract_quoted_json_string_after_key(next_obj, "\"author\"", false);
            }
        }
    }

    if ((next_title && next_title[0] != '\0') || (next_artist && next_artist[0] != '\0')) {
        copy_string_field(state->next_title, sizeof(state->next_title), next_title);
        copy_string_field(state->next_artist, sizeof(state->next_artist), next_artist);
        state->has_next_song = true;
    }

    free(next_title);
    free(next_artist);
}

static void extract_playback_state_from_song_json(const uint8_t *json_data,
                                                  size_t json_len,
                                                  ytmd_client_playback_state_t *state)
{
    if (!json_data || json_len == 0 || !state) {
        return;
    }

    char *json = (char *)malloc(json_len + 1);
    if (!json) {
        return;
    }
    memcpy(json, json_data, json_len);
    json[json_len] = '\0';

    bool v = false;
    if (extract_json_bool_after_key(json, "\"isPlaying\"", &v)) {
        state->has_playing = true;
        state->is_playing = v;
    }
    if (extract_json_bool_after_key(json, "\"isPaused\"", &v) ||
        extract_json_bool_after_key(json, "\"paused\"", &v)) {
        state->has_paused = true;
        state->is_paused = v;
        // isPaused is a direct state signal; mirror to play-state for existing UI path.
        state->has_playing = true;
        state->is_playing = !v;
    }

    if (parse_shuffle_state_from_json(json, &v)) {
        state->has_shuffle = true;
        state->is_shuffle = v;
    }

    ytmd_client_repeat_t repeat = YTMD_CLIENT_REPEAT_NONE;
    if (parse_repeat_state_from_json(json, &repeat)) {
        state->has_repeat = true;
        state->repeat = repeat;
    }

    (void)parse_like_state_from_json(json, state);

    int seek_seconds = 0;
    if (extract_seek_seconds_from_song_json(json, &seek_seconds)) {
        state->has_seek_seconds = true;
        state->seek_seconds = seek_seconds;
        state->has_elapsed_seconds = true;
        state->elapsed_seconds = seek_seconds;
    }

    int song_duration_seconds = 0;
    if (extract_song_duration_seconds_from_song_json(json, &song_duration_seconds)) {
        state->has_song_duration_seconds = true;
        state->song_duration_seconds = song_duration_seconds;
    }

    extract_next_song_from_song_json(json, state);
    free(json);
}

static void enrich_playback_state_from_endpoint(const char *url,
                                                ytmd_client_playback_state_t *state,
                                                ytmd_network_diag_cb_t net_diag_cb)
{
    if (!url || !state) {
        return;
    }

    uint8_t *json = NULL;
    size_t json_len = 0;
    esp_err_t err = http_get_alloc(url,
                                   MAX_STATE_JSON_BYTES,
                                   HTTP_STATE_TIMEOUT_MS,
                                   true,
                                   net_diag_cb,
                                   NULL,
                                   NULL,
                                   &json,
                                   &json_len);
    if (err == ESP_OK && json && json_len > 0) {
        extract_playback_state_from_song_json(json, json_len, state);
    }
    free(json);
}

static void enrich_next_song_from_queue_endpoints(ytmd_client_playback_state_t *state,
                                                  ytmd_network_diag_cb_t net_diag_cb)
{
    if (!state || state->has_next_song) {
        if (state && state->has_next_song) {
            ESP_LOGD(TAG, "NEXT: /song already has next (title='%s', artist='%s'), skip queue fetch",
                     state->next_title, state->next_artist);
        }
        return;
    }

    if (s_queue_next_supported) {
        ESP_LOGD(TAG, "NEXT: request /api/v1/queue/next");
        uint8_t *queue_next_json = NULL;
        size_t queue_next_json_len = 0;
        int queue_next_status = 0;
        esp_err_t err = http_get_alloc(YTMD_URL_API_QUEUE_NEXT,
                                       MAX_QUEUE_NEXT_JSON_BYTES,
                                       HTTP_QUEUE_TIMEOUT_MS,
                                       true,
                                       net_diag_cb,
                                       &queue_next_status,
                                       NULL,
                                       &queue_next_json,
                                       &queue_next_json_len);
        ESP_LOGD(TAG, "NEXT: /queue/next result err=%s http=%d bytes=%u",
                 esp_err_to_name(err), queue_next_status, (unsigned)queue_next_json_len);
        if (err == ESP_OK && queue_next_json && queue_next_json_len > 0) {
            if (extract_next_song_from_queue_next_json(queue_next_json,
                                                       queue_next_json_len,
                                                       state->next_title,
                                                       sizeof(state->next_title),
                                                       state->next_artist,
                                                       sizeof(state->next_artist))) {
                state->has_next_song = true;
                log_next_track_if_changed("/queue/next", state->next_title, state->next_artist);
            } else {
                ESP_LOGD(TAG, "NEXT: parse miss on /queue/next payload");
            }
        } else if (queue_next_status == 404 || queue_next_status == 405) {
            s_queue_next_supported = false;
            ESP_LOGW(TAG, "Disabling /queue/next enrichment (HTTP %d), keeping /queue fallback", queue_next_status);
        }
        free(queue_next_json);
    }

    if (!state->has_next_song && s_queue_fallback_enabled) {
        ESP_LOGD(TAG, "NEXT: request /api/v1/queue");
        uint8_t *queue_json = NULL;
        size_t queue_json_len = 0;
        bool queue_too_large = false;
        int queue_status = 0;
        esp_err_t err = http_get_alloc(YTMD_URL_API_QUEUE,
                                       MAX_QUEUE_JSON_BYTES,
                                       HTTP_QUEUE_TIMEOUT_MS,
                                       true,
                                       net_diag_cb,
                                       &queue_status,
                                       &queue_too_large,
                                       &queue_json,
                                       &queue_json_len);
        ESP_LOGD(TAG, "NEXT: /queue result err=%s http=%d bytes=%u",
                 esp_err_to_name(err), queue_status, (unsigned)queue_json_len);
        if (err == ESP_OK && queue_json && queue_json_len > 0) {
            if (extract_next_song_from_queue_json(queue_json,
                                                  queue_json_len,
                                                  state->next_title,
                                                  sizeof(state->next_title),
                                                  state->next_artist,
                                                  sizeof(state->next_artist))) {
                state->has_next_song = true;
                log_next_track_if_changed("/queue", state->next_title, state->next_artist);
            } else {
                ESP_LOGD(TAG, "NEXT: parse miss on /queue payload");
            }
        } else if (queue_too_large) {
            ESP_LOGW(TAG, "NEXT: /queue payload exceeded %u bytes (will retry next poll)", (unsigned)MAX_QUEUE_JSON_BYTES);
        }
        free(queue_json);
    }

    if (!state->has_next_song) {
        ESP_LOGD(TAG, "NEXT: no next-song info after queue enrichment");
    }
}

static bool extract_next_song_from_queue_json(const uint8_t *json_data,
                                              size_t json_len,
                                              char *out_title,
                                              size_t out_title_cap,
                                              char *out_artist,
                                              size_t out_artist_cap)
{
    if (!json_data || json_len == 0 || !out_title || out_title_cap == 0 || !out_artist || out_artist_cap == 0) {
        return false;
    }

    out_title[0] = '\0';
    out_artist[0] = '\0';

    char *json = (char *)malloc(json_len + 1);
    if (!json) {
        return false;
    }
    memcpy(json, json_data, json_len);
    json[json_len] = '\0';

    bool cache_locked = false;
    bool cache_enabled = false;
    bool cache_overflow_logged = false;
    if (queue_cache_lock(pdMS_TO_TICKS(20))) {
        cache_locked = true;
        if (ensure_queue_cache_alloc()) {
            cache_enabled = true;
            s_queue_cache_count = 0;
            s_queue_cache_selected_pos = -1;
        }
    }

    const char *scan = strstr(json, "\"items\"");
    if (!scan) {
        scan = json;
    }

    while ((scan = strstr(scan, "\"playlistPanelVideoRenderer\"")) != NULL) {
        const char *colon = strchr(scan + strlen("\"playlistPanelVideoRenderer\""), ':');
        if (!colon) {
            break;
        }

        const char *renderer_start = skip_json_ws(colon + 1);
        if (!renderer_start || *renderer_start != '{') {
            scan = colon + 1;
            continue;
        }

        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        const char *renderer_end = NULL;
        for (const char *p = renderer_start; *p; ++p) {
            char c = *p;
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }

            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == '{') {
                depth++;
                continue;
            }
            if (c == '}') {
                depth--;
                if (depth == 0) {
                    renderer_end = p;
                    break;
                }
            }
        }

        if (!renderer_end || renderer_end <= renderer_start) {
            break;
        }

        size_t renderer_len = (size_t)(renderer_end - renderer_start + 1);
        char *renderer_json = (char *)malloc(renderer_len + 1);
        if (!renderer_json) {
            break;
        }
        memcpy(renderer_json, renderer_start, renderer_len);
        renderer_json[renderer_len] = '\0';

        bool is_selected = false;
        (void)extract_json_bool_after_key(renderer_json, "\"selected\"", &is_selected);
        int row_index = -1;
        (void)extract_json_int_after_key(renderer_json, "\"index\"", &row_index);

        char *video_id_token = extract_quoted_json_string_after_key(renderer_json, "\"videoId\"", false);
        char *video_id = dup_video_id_if_valid(video_id_token);
        free(video_id_token);

        if (!video_id) {
            const char *watch_obj = strstr(renderer_json, "\"watchEndpoint\"");
            if (watch_obj) {
                video_id_token = extract_quoted_json_string_after_key(watch_obj, "\"videoId\"", false);
                video_id = dup_video_id_if_valid(video_id_token);
                free(video_id_token);
            }
        }

        char *thumb_url = NULL;
        const char *thumb_obj = strstr(renderer_json, "\"thumbnail\"");
        if (thumb_obj) {
            thumb_url = extract_quoted_json_string_after_key(thumb_obj, "\"url\"", true);
        }
        if (!thumb_url) {
            thumb_url = extract_quoted_json_string_after_key(renderer_json, "\"url\"", true);
        }

        char *title = NULL;
        const char *title_obj = strstr(renderer_json, "\"title\"");
        if (title_obj) {
            title = extract_quoted_json_string_after_key(title_obj, "\"text\"", false);
        }
        if (!title || title[0] == '\0') {
            free(title);
            title = extract_quoted_json_string_after_key(renderer_json, "\"title\"", false);
        }

        char *artist = NULL;
        const char *short_byline_obj = strstr(renderer_json, "\"shortBylineText\"");
        if (short_byline_obj) {
            artist = extract_quoted_json_string_after_key(short_byline_obj, "\"text\"", false);
        }
        if (!artist || artist[0] == '\0') {
            free(artist);
            artist = NULL;
            const char *long_byline_obj = strstr(renderer_json, "\"longBylineText\"");
            if (long_byline_obj) {
                artist = extract_quoted_json_string_after_key(long_byline_obj, "\"text\"", false);
            }
        }
        if (!artist || artist[0] == '\0') {
            free(artist);
            artist = extract_quoted_json_string_after_key(renderer_json, "\"artist\"", false);
        }
        if (!artist || artist[0] == '\0') {
            free(artist);
            artist = extract_quoted_json_string_after_key(renderer_json, "\"author\"", false);
        }

        const bool has_track = ((title && title[0] != '\0') ||
                                (artist && artist[0] != '\0') ||
                                (video_id && video_id[0] != '\0') ||
                                (thumb_url && thumb_url[0] != '\0'));
        if (cache_enabled && has_track) {
            int duplicate_pos = -1;
            for (size_t i = 0; i < s_queue_cache_count; ++i) {
                const ytmd_client_queue_item_t *entry = &s_queue_cache_items[i];
                bool same_video = (video_id && video_id[0] != '\0' &&
                                   entry->video_id[0] != '\0' &&
                                   strncmp(entry->video_id, video_id, sizeof(entry->video_id) - 1) == 0);
                bool same_row = (row_index >= 0 && entry->index >= 0 && entry->index == row_index);
                bool same_text = (title && title[0] != '\0' &&
                                  artist && artist[0] != '\0' &&
                                  entry->title[0] != '\0' &&
                                  entry->artist[0] != '\0' &&
                                  text_equal_norm_alnum(entry->title, title) &&
                                  text_equal_norm_alnum(entry->artist, artist));
                if (same_video || (same_row && same_text)) {
                    duplicate_pos = (int)i;
                    break;
                }
            }

            if (duplicate_pos >= 0) {
                if (is_selected) {
                    s_queue_cache_selected_pos = duplicate_pos;
                    for (size_t i = 0; i < s_queue_cache_count; ++i) {
                        s_queue_cache_items[i].selected = ((int)i == duplicate_pos);
                    }
                }
            } else if (s_queue_cache_count < YTMD_QUEUE_CACHE_CAPACITY) {
                ytmd_client_queue_item_t *item = &s_queue_cache_items[s_queue_cache_count];
                memset(item, 0, sizeof(*item));
                item->index = (row_index >= 0) ? row_index : (int)s_queue_cache_count;
                item->selected = is_selected;
                copy_string_field(item->title, sizeof(item->title), title);
                copy_string_field(item->artist, sizeof(item->artist), artist);
                copy_string_field(item->video_id, sizeof(item->video_id), video_id);
                copy_string_field(item->art_url, sizeof(item->art_url), thumb_url);
                if (is_selected && s_queue_cache_selected_pos < 0) {
                    s_queue_cache_selected_pos = (int)s_queue_cache_count;
                }
                s_queue_cache_count++;
            } else if (!cache_overflow_logged) {
                ESP_LOGW(TAG, "QUEUE CACHE: capacity reached (%u items), truncating",
                         (unsigned)YTMD_QUEUE_CACHE_CAPACITY);
                cache_overflow_logged = true;
            }
        }

        free(title);
        free(artist);
        free(video_id);
        free(thumb_url);
        free(renderer_json);
        scan = renderer_end + 1;
    }

    bool fallback_ok = false;
    if (cache_locked) {
        if (s_queue_cache_selected_pos < 0) {
            int hint_pos = queue_cache_find_selected_pos_by_hint_locked(
                s_track_hint_title,
                s_track_hint_artist,
                s_track_hint_video_id);
            if (hint_pos >= 0) {
                s_queue_cache_selected_pos = hint_pos;
                for (size_t i = 0; i < s_queue_cache_count; ++i) {
                    s_queue_cache_items[i].selected = ((int)i == hint_pos);
                }
            }
        }
        ESP_LOGD(TAG, "NEXT: queue-cache resolve selected_pos=%d count=%u hint_title='%s' hint_artist='%s' hint_vid='%s'",
                 s_queue_cache_selected_pos,
                 (unsigned)s_queue_cache_count,
                 s_track_hint_title,
                 s_track_hint_artist,
                 s_track_hint_video_id);
        fallback_ok = queue_cache_copy_next_from_selected_locked(
            out_title,
            out_title_cap,
            out_artist,
            out_artist_cap);
        queue_cache_unlock();
    }
    free(json);
    return fallback_ok;
}

static bool extract_next_song_from_queue_next_json(const uint8_t *json_data,
                                                   size_t json_len,
                                                   char *out_title,
                                                   size_t out_title_cap,
                                                   char *out_artist,
                                                   size_t out_artist_cap)
{
    if (!json_data || json_len == 0 || !out_title || out_title_cap == 0 || !out_artist || out_artist_cap == 0) {
        return false;
    }

    out_title[0] = '\0';
    out_artist[0] = '\0';

    char *json = (char *)malloc(json_len + 1);
    if (!json) {
        return false;
    }
    memcpy(json, json_data, json_len);
    json[json_len] = '\0';

    char *title = extract_quoted_json_string_after_key(json, "\"title\"", false);
    if (!title || title[0] == '\0') {
        free(title);
        title = NULL;
        const char *title_obj = strstr(json, "\"title\"");
        if (title_obj) {
            title = extract_quoted_json_string_after_key(title_obj, "\"text\"", false);
        }
    }

    char *artist = NULL;
    const char *byline_obj = strstr(json, "\"shortBylineText\"");
    if (byline_obj) {
        artist = extract_quoted_json_string_after_key(byline_obj, "\"text\"", false);
    }
    if (!artist || artist[0] == '\0') {
        free(artist);
        artist = extract_quoted_json_string_after_key(json, "\"artist\"", false);
    }
    if (!artist || artist[0] == '\0') {
        free(artist);
        artist = extract_quoted_json_string_after_key(json, "\"author\"", false);
    }

    bool ok = false;
    if ((title && title[0] != '\0') || (artist && artist[0] != '\0')) {
        copy_string_field(out_title, out_title_cap, title);
        copy_string_field(out_artist, out_artist_cap, artist);
        ok = true;
    }

    free(title);
    free(artist);
    free(json);
    return ok;
}

static esp_err_t http_get_alloc(const char *url,
                                size_t max_size,
                                int timeout_ms,
                                bool text_mode,
                                ytmd_network_diag_cb_t net_diag_cb,
                                int *out_status,
                                bool *out_too_large,
                                uint8_t **out_buf,
                                size_t *out_len)
{
    ESP_RETURN_ON_FALSE(url && out_buf && out_len, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    *out_buf = NULL;
    *out_len = 0;
    if (out_status) {
        *out_status = 0;
    }
    if (out_too_large) {
        *out_too_large = false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .addr_type = HTTP_ADDR_TYPE_INET, // Force IPv4 on USB-NCM path to avoid IPv6 connect timeouts.
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "esp_http_client_init failed");
    if (!text_mode) {
        // Force JPEG-first negotiation so google-hosted art doesn't return WEBP/PNG unexpectedly.
        (void)esp_http_client_set_header(client, "Accept", "image/jpeg");
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
    if (out_status) {
        *out_status = status;
    }
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
    if (content_len > 0 && (size_t)content_len <= max_size) {
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
        size_t payload_need = used + (size_t)read_len;
        size_t need = payload_need + (text_mode ? 1u : 0u);
        if (payload_need > max_size) {
            ESP_LOGW(TAG, "HTTP body too large: %u > %u", (unsigned)payload_need, (unsigned)max_size);
            if (out_too_large) {
                *out_too_large = true;
            }
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

static esp_err_t http_send_json(const char *url,
                                esp_http_client_method_t method,
                                const char *body,
                                int timeout_ms,
                                ytmd_network_diag_cb_t net_diag_cb)
{
    ESP_RETURN_ON_FALSE(url, ESP_ERR_INVALID_ARG, TAG, "url is NULL");

    const char *payload = body ? body : "{}";
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .addr_type = HTTP_ADDR_TYPE_INET, // Keep command channel consistent with album-art fetch transport.
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "esp_http_client_init failed");
    (void)esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, (int)strlen(payload));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP %s open failed for %s: %s",
                 method == HTTP_METHOD_PATCH ? "PATCH" : "POST",
                 url,
                 esp_err_to_name(err));
        if (url && strncmp(url, "https://", 8) == 0 && net_diag_cb) {
            net_diag_cb(url);
        }
        esp_http_client_cleanup(client);
        return err;
    }

    int written = esp_http_client_write(client, payload, (int)strlen(payload));
    if (written < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %s status=%d for %s",
                 method == HTTP_METHOD_PATCH ? "PATCH" : "POST",
                 status,
                 url);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t http_post_json(const char *url, const char *body, int timeout_ms, ytmd_network_diag_cb_t net_diag_cb)
{
    return http_send_json(url, HTTP_METHOD_POST, body, timeout_ms, net_diag_cb);
}

static esp_err_t http_patch_json(const char *url, const char *body, int timeout_ms, ytmd_network_diag_cb_t net_diag_cb)
{
    return http_send_json(url, HTTP_METHOD_PATCH, body, timeout_ms, net_diag_cb);
}

static esp_err_t ytmd_post_with_fallback(const char *path_primary,
                                         const char *body_primary,
                                         const char *path_fallback,
                                         const char *body_fallback,
                                         ytmd_network_diag_cb_t net_diag_cb)
{
    char url_primary[160] = {0};
    snprintf(url_primary, sizeof(url_primary), "%s%s", YTMD_API_BASE, path_primary);
    esp_err_t err = http_post_json(url_primary, body_primary, HTTP_CMD_TIMEOUT_MS, net_diag_cb);
    if (err == ESP_OK || !path_fallback || path_fallback[0] == '\0') {
        return err;
    }

    char url_fallback[160] = {0};
    snprintf(url_fallback, sizeof(url_fallback), "%s%s", YTMD_API_BASE, path_fallback);
    return http_post_json(url_fallback, body_fallback, HTTP_CMD_TIMEOUT_MS, net_diag_cb);
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

static void scale_crop_zoom_rgb565(const uint16_t *src,
                                   int src_w,
                                   int src_h,
                                   int src_stride,
                                   uint16_t *dst,
                                   int dst_w,
                                   int dst_h,
                                   int zoom_percent)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || src_stride < src_w || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    if (zoom_percent < 100) {
        zoom_percent = 100;
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

    if (zoom_percent > 100) {
        int zoom_w = (int)((int64_t)crop_w * 100 / zoom_percent);
        int zoom_h = (int)((int64_t)crop_h * 100 / zoom_percent);
        if (zoom_w < 1) {
            zoom_w = 1;
        }
        if (zoom_h < 1) {
            zoom_h = 1;
        }

        int center_x = crop_x + crop_w / 2;
        int center_y = crop_y + crop_h / 2;
        crop_w = zoom_w;
        crop_h = zoom_h;
        crop_x = center_x - crop_w / 2;
        crop_y = center_y - crop_h / 2;

        if (crop_x < 0) {
            crop_x = 0;
        }
        if (crop_y < 0) {
            crop_y = 0;
        }
        if (crop_x + crop_w > src_w) {
            crop_x = src_w - crop_w;
        }
        if (crop_y + crop_h > src_h) {
            crop_y = src_h - crop_h;
        }
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
                                      char *out_title,
                                      size_t out_title_cap,
                                      char *out_artist,
                                      size_t out_artist_cap,
                                      ytmd_client_playback_state_t *out_playback_state,
                                      ytmd_network_diag_cb_t net_diag_cb)
{
    ESP_RETURN_ON_FALSE(dst_rgb565 && dst_w > 0 && dst_h > 0, ESP_ERR_INVALID_ARG, TAG, "invalid destination");
    ESP_RETURN_ON_FALSE(out_art_url && out_art_url_cap > 0, ESP_ERR_INVALID_ARG, TAG, "invalid out_art_url");
    out_art_url[0] = '\0';
    if (out_title && out_title_cap > 0) {
        out_title[0] = '\0';
    }
    if (out_artist && out_artist_cap > 0) {
        out_artist[0] = '\0';
    }
    if (out_playback_state) {
        init_playback_state(out_playback_state);
    }

    uint8_t *json = NULL;
    size_t json_len = 0;
    esp_err_t err = http_get_alloc(
        YTMD_URL_API_SONG,
        MAX_SONG_JSON_BYTES,
        HTTP_TIMEOUT_MS,
        true,
        net_diag_cb,
        NULL,
        NULL,
        &json,
        &json_len);
    if (err != ESP_OK) {
        return err;
    }

    extract_song_title_artist_from_json(json, json_len, out_title, out_title_cap, out_artist, out_artist_cap);
    if (out_playback_state) {
        extract_playback_state_from_song_json(json, json_len, out_playback_state);
        if (!out_playback_state->has_next_song) {
            (void)fill_next_song_from_queue_cache(out_playback_state);
        }
    }

    char *art_url = extract_art_url_from_song_json(json, json_len);
    char *video_id = extract_video_id_from_song_json(json, json_len);
    queue_cache_set_track_hint(out_title, out_artist, video_id);
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

    char art_cache_key[YTMD_ART_URL_MAX_LEN] = {0};
    build_art_cache_key(art_url, video_id, art_cache_key, sizeof(art_cache_key));
    if (art_cache_key[0] == '\0') {
        snprintf(art_cache_key, sizeof(art_cache_key), "%s", art_url);
    }

    if (last_art_url && last_art_url[0] != '\0' && strncmp(art_cache_key, last_art_url, YTMD_ART_URL_MAX_LEN - 1) == 0) {
        free(art_url);
        free(video_id);
        return ESP_ERR_NOT_FOUND;
    }

    if (art_cache_copy_to_dst(art_cache_key, dst_w, dst_h, dst_rgb565)) {
        snprintf(out_art_url, out_art_url_cap, "%s", art_cache_key);
        schedule_prefetch_art_window_from_queue(art_cache_key, dst_w, dst_h, net_diag_cb);
        free(art_url);
        free(video_id);
        return ESP_OK;
    }

    char fb_url[YTMD_ART_URL_MAX_LEN] = {0};
    bool has_fallback = make_ytimg_fallback_url(video_id, fb_url, sizeof(fb_url));
    bool using_fallback_url = has_fallback && (strcmp(art_url, fb_url) == 0);
    const bool has_exact_square_hint = has_square_art_size_hint(art_url);
    const bool prefer_fallback_first = has_fallback &&
                                       is_googleusercontent_art_url(art_url) &&
                                       !has_exact_square_hint;
    ESP_LOGI(TAG, "Art URL selected=%s cache_key=%s videoId=%s fallback=%s prefer_fallback_first=%d",
             art_url,
             art_cache_key,
             video_id ? video_id : "(none)",
             has_fallback ? fb_url : "(none)",
             prefer_fallback_first ? 1 : 0);

    uint8_t *img = NULL;
    size_t img_len = 0;
    if (prefer_fallback_first) {
        ESP_LOGW(TAG, "Trying fallback first for googleusercontent URL: %s", fb_url);
        err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, NULL, NULL, &img, &img_len);
        if (err == ESP_OK) {
            char *new_url = dup_cstr(fb_url);
            if (new_url) {
                free(art_url);
                art_url = new_url;
                using_fallback_url = true;
            }
        } else {
            ESP_LOGW(TAG, "Fallback-first failed, trying primary URL: %s", art_url);
            err = http_get_alloc(art_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, NULL, NULL, &img, &img_len);
        }
    } else {
        err = http_get_alloc(art_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, NULL, NULL, &img, &img_len);
        if (err != ESP_OK && has_fallback && !using_fallback_url) {
            ESP_LOGW(TAG, "Primary art URL failed, trying fallback: %s", fb_url);
            err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, NULL, NULL, &img, &img_len);
            if (err == ESP_OK) {
                char *new_url = dup_cstr(fb_url);
                if (new_url) {
                    free(art_url);
                    art_url = new_url;
                    using_fallback_url = true;
                }
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
        const bool png = is_png_data(img, img_len);
        const bool webp = is_webp_data(img, img_len);
        ESP_LOGW(TAG, "Art payload is not JPEG (url=%s fmt=%s magic=%02X %02X %02X %02X)",
                 art_url,
                 png ? "PNG" : (webp ? "WEBP" : "UNKNOWN"),
                 img_len > 0 ? img[0] : 0,
                 img_len > 1 ? img[1] : 0,
                 img_len > 2 ? img[2] : 0,
                 img_len > 3 ? img[3] : 0);
        if (has_fallback && !using_fallback_url) {
            ESP_LOGW(TAG, "Primary art is not JPEG, retry with fallback: %s", fb_url);
            free(img);
            img = NULL;
            img_len = 0;
            err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, NULL, NULL, &img, &img_len);
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
        err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, net_diag_cb, NULL, NULL, &img, &img_len);
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

    int crop_zoom_percent = 100;
    if (using_fallback_url && dst_w == dst_h && w > 0 && h > 0) {
        int max_dim = (w > h) ? w : h;
        int min_dim = (w > h) ? h : w;
        // Fallback thumbnails are often 4:3/16:9 with a smaller centered album image.
        // Add extra center zoom so PNG/WEBP->JPEG fallback looks closer to square cover art framing.
        if ((int64_t)max_dim * 10 >= (int64_t)min_dim * 12) {
            crop_zoom_percent = 145;
        }
    }
    scale_crop_zoom_rgb565(decoded, w, h, stride, dst_rgb565, dst_w, dst_h, crop_zoom_percent);
    free(decoded);

    (void)art_cache_store_from_src(art_cache_key, dst_w, dst_h, dst_rgb565);
    schedule_prefetch_art_window_from_queue(art_cache_key, dst_w, dst_h, net_diag_cb);

    snprintf(out_art_url, out_art_url_cap, "%s", art_cache_key);
    free(art_url);
    free(video_id);

    ESP_LOGI(TAG, "Album art decoded: src=%dx%d -> dst=%dx%d mode=crop zoom=%d%%", w, h, dst_w, dst_h, crop_zoom_percent);
    return ESP_OK;
}

esp_err_t ytmd_client_queue_cache_get(ytmd_client_queue_item_t *out_items,
                                      size_t max_items,
                                      size_t *out_copied,
                                      size_t *out_total,
                                      int *out_selected_pos)
{
    if (out_copied) {
        *out_copied = 0;
    }
    if (out_total) {
        *out_total = 0;
    }
    if (out_selected_pos) {
        *out_selected_pos = -1;
    }

    if (!queue_cache_lock(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }

    size_t total = s_queue_cache_count;
    size_t copied = 0;
    if (out_items && max_items > 0 && s_queue_cache_items && total > 0) {
        copied = (total < max_items) ? total : max_items;
        memcpy(out_items, s_queue_cache_items, copied * sizeof(ytmd_client_queue_item_t));
    }

    if (out_copied) {
        *out_copied = copied;
    }
    if (out_total) {
        *out_total = total;
    }
    if (out_selected_pos) {
        *out_selected_pos = s_queue_cache_selected_pos;
    }

    queue_cache_unlock();
    return ESP_OK;
}

esp_err_t ytmd_client_queue_cache_get_compact(ytmd_client_queue_compact_item_t *out_items,
                                              size_t max_items,
                                              size_t *out_copied,
                                              size_t *out_total,
                                              int *out_selected_pos)
{
    if (out_copied) {
        *out_copied = 0;
    }
    if (out_total) {
        *out_total = 0;
    }
    if (out_selected_pos) {
        *out_selected_pos = -1;
    }

    if (!queue_cache_lock(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }

    size_t total = s_queue_cache_count;
    size_t copied = 0;
    if (out_items && max_items > 0 && s_queue_cache_items && total > 0) {
        copied = (total < max_items) ? total : max_items;
        for (size_t i = 0; i < copied; ++i) {
            const ytmd_client_queue_item_t *src = &s_queue_cache_items[i];
            ytmd_client_queue_compact_item_t *dst = &out_items[i];
            dst->index = src->index;
            dst->selected = src->selected;
            copy_string_field(dst->title, sizeof(dst->title), src->title);
            copy_string_field(dst->artist, sizeof(dst->artist), src->artist);
            copy_string_field(dst->video_id, sizeof(dst->video_id), src->video_id);
        }
    }

    if (out_copied) {
        *out_copied = copied;
    }
    if (out_total) {
        *out_total = total;
    }
    if (out_selected_pos) {
        *out_selected_pos = s_queue_cache_selected_pos;
    }

    queue_cache_unlock();
    return ESP_OK;
}

esp_err_t ytmd_client_refresh_queue_cache(ytmd_network_diag_cb_t net_diag_cb)
{
    uint8_t *queue_json = NULL;
    size_t queue_json_len = 0;
    bool too_large = false;
    int queue_status = 0;

    esp_err_t err = http_get_alloc(
        YTMD_URL_API_QUEUE,
        MAX_QUEUE_JSON_BYTES,
        HTTP_QUEUE_TIMEOUT_MS,
        true,
        net_diag_cb,
        &queue_status,
        &too_large,
        &queue_json,
        &queue_json_len);

    if (err != ESP_OK) {
        if (too_large) {
            ESP_LOGW(TAG, "QUEUE refresh payload exceeded %u bytes", (unsigned)MAX_QUEUE_JSON_BYTES);
            return ESP_ERR_INVALID_SIZE;
        }
        return err;
    }

    if (!queue_json || queue_json_len == 0) {
        free(queue_json);
        return ESP_ERR_NOT_FOUND;
    }

    char dummy_title[256] = {0};
    char dummy_artist[128] = {0};
    (void)extract_next_song_from_queue_json(queue_json,
                                            queue_json_len,
                                            dummy_title,
                                            sizeof(dummy_title),
                                            dummy_artist,
                                            sizeof(dummy_artist));
    free(queue_json);
    return ESP_OK;
}

esp_err_t ytmd_client_try_get_cached_art_by_queue_offset(int rel_offset,
                                                         uint16_t *dst_rgb565,
                                                         int dst_w,
                                                         int dst_h,
                                                         char *out_art_key,
                                                         size_t out_art_key_cap,
                                                         char *out_title,
                                                         size_t out_title_cap,
                                                         char *out_artist,
                                                         size_t out_artist_cap)
{
    if (!dst_rgb565 || dst_w <= 0 || dst_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_art_key && out_art_key_cap > 0) {
        out_art_key[0] = '\0';
    }
    if (out_title && out_title_cap > 0) {
        out_title[0] = '\0';
    }
    if (out_artist && out_artist_cap > 0) {
        out_artist[0] = '\0';
    }

    if (!queue_cache_lock(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_queue_cache_items || s_queue_cache_count == 0 || s_queue_cache_selected_pos < 0) {
        queue_cache_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    int idx = s_queue_cache_selected_pos + rel_offset;
    if (idx < 0 || (size_t)idx >= s_queue_cache_count) {
        queue_cache_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    const ytmd_client_queue_item_t *item = &s_queue_cache_items[idx];
    char art_key[YTMD_ART_URL_MAX_LEN] = {0};
    if (item->video_id[0] != '\0') {
        char fallback_url[YTMD_ART_URL_MAX_LEN] = {0};
        if (make_ytimg_fallback_url(item->video_id, fallback_url, sizeof(fallback_url))) {
            build_art_cache_key(fallback_url, item->video_id, art_key, sizeof(art_key));
        }
    }
    if (art_key[0] == '\0' && item->art_url[0] != '\0') {
        build_art_cache_key(item->art_url, NULL, art_key, sizeof(art_key));
    }

    if (out_title && out_title_cap > 0) {
        snprintf(out_title, out_title_cap, "%s", item->title);
    }
    if (out_artist && out_artist_cap > 0) {
        snprintf(out_artist, out_artist_cap, "%s", item->artist);
    }
    queue_cache_unlock();

    if (art_key[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    if (!art_cache_copy_to_dst(art_key, dst_w, dst_h, dst_rgb565)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (out_art_key && out_art_key_cap > 0) {
        snprintf(out_art_key, out_art_key_cap, "%s", art_key);
    }
    return ESP_OK;
}

esp_err_t ytmd_client_enrich_playback_state(ytmd_client_playback_state_t *state,
                                            ytmd_network_diag_cb_t net_diag_cb)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    enrich_playback_state_from_endpoint(YTMD_URL_API_SHUFFLE, state, net_diag_cb);
    enrich_playback_state_from_endpoint(YTMD_URL_API_REPEAT_MODE, state, net_diag_cb);
    enrich_playback_state_from_endpoint(YTMD_URL_API_LIKE_STATE, state, net_diag_cb);
    enrich_next_song_from_queue_endpoints(state, net_diag_cb);
    if (!state->has_next_song) {
        (void)fill_next_song_from_queue_cache(state);
    }
    return ESP_OK;
}

esp_err_t ytmd_client_cmd_play_pause(void)
{
    return ytmd_post_with_fallback("/api/v1/toggle-play",
                                   "{}",
                                   "/api/v1/play-pause",
                                   "{}",
                                   NULL);
}

esp_err_t ytmd_client_cmd_prev(void)
{
    return ytmd_post_with_fallback("/api/v1/previous", "{}", NULL, NULL, NULL);
}

esp_err_t ytmd_client_cmd_next(void)
{
    // Keep legacy route first, then try known alternates used by some YTMD builds.
    esp_err_t err = ytmd_post_with_fallback("/api/v1/next", "{}", "/api/v1/next-song", "{}", NULL);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    // Companion-style fallback (best-effort; may be unavailable if auth is required).
    return ytmd_post_with_fallback("/api/v1/command",
                                   "{\"command\":\"next\"}",
                                   NULL,
                                   NULL,
                                   NULL);
}

esp_err_t ytmd_client_cmd_play_queue_index(int index)
{
    if (index < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[48] = {0};
    snprintf(body, sizeof(body), "{\"index\":%d}", index);

    char url[160] = {0};
    snprintf(url, sizeof(url), "%s%s", YTMD_API_BASE, "/api/v1/queue");
    return http_patch_json(url, body, HTTP_CMD_TIMEOUT_MS, NULL);
}

esp_err_t ytmd_client_cmd_play_video_id(const char *video_id)
{
    if (!video_id || !is_valid_youtube_video_id(video_id, strlen(video_id))) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[128] = {0};
    snprintf(body,
             sizeof(body),
             "{\"videoId\":\"%s\",\"insertPosition\":\"INSERT_AFTER_CURRENT_VIDEO\"}",
             video_id);

    esp_err_t err = ytmd_post_with_fallback("/api/v1/queue", body, NULL, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    return ytmd_client_cmd_next();
}

esp_err_t ytmd_client_cmd_seek_to(int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }

    char seek_body[48] = {0};
    snprintf(seek_body, sizeof(seek_body), "{\"seconds\":%d}", seconds);

    esp_err_t err = ytmd_post_with_fallback("/api/v1/seek-to", seek_body, NULL, NULL, NULL);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    // Some YTMD builds accept seek target in path.
    char seek_path[64] = {0};
    snprintf(seek_path, sizeof(seek_path), "/api/v1/seek-to/%d", seconds);
    return ytmd_post_with_fallback(seek_path, "{}", NULL, NULL, NULL);
}

esp_err_t ytmd_client_cmd_toggle_shuffle(void)
{
    return ytmd_post_with_fallback("/api/v1/shuffle", "{}", NULL, NULL, NULL);
}

esp_err_t ytmd_client_cmd_cycle_repeat(void)
{
    return ytmd_post_with_fallback("/api/v1/switch-repeat", "{\"iteration\":1}", NULL, NULL, NULL);
}

esp_err_t ytmd_client_cmd_toggle_like(void)
{
    return ytmd_post_with_fallback("/api/v1/like", "{}", NULL, NULL, NULL);
}

esp_err_t ytmd_client_cmd_toggle_dislike(void)
{
    return ytmd_post_with_fallback("/api/v1/dislike", "{}", NULL, NULL, NULL);
}
