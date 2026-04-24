#include "player_ui.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"

#include "bsp/esp-bsp.h"

#include "ui/images.h"
#include "ui/screens.h"
#include "ui/ui.h"

#include "ui_display.h"
#include "ytmd_client.h"

static const char *TAG = "PLAYER_UI";

#define PLAYER_NEXT_LABEL_TEXT "Next"
#define PLAYER_ALBUM_ART_RADIUS 6
#define PLAYER_SKIP_PUSH_TIMEOUT_MS 5000U
#define PLAYER_PREV_REWIND_THRESHOLD_SEC 5
#define PLAYER_BG_ART_OPA LV_OPA_30
#define PLAYER_BG_ART_RECOLOR_OPA LV_OPA_20
#define PLAYER_SEEKBAR_BG_OPA LV_OPA_30
#define PLAYER_SEEKBAR_FALLBACK_COLOR_HEX 0x6A6A6A
#define PLAYER_SEEKBAR_MIN_LUMA 55

typedef struct {
    lv_obj_t *album_art;
    lv_obj_t *song_title;
    lv_obj_t *song_artist;
    lv_obj_t *play;
    lv_obj_t *skip_prev;
    lv_obj_t *skip_next;
    lv_obj_t *song_random;
    lv_obj_t *song_repeat;
    lv_obj_t *song_like;
    lv_obj_t *song_senti;
    lv_obj_t *next;
    lv_obj_t *next_song;
    lv_obj_t *seekbar;
    lv_obj_t *time_now;
    lv_obj_t *total_time;
} player_ui_objects_t;

static player_ui_objects_t s_ui;
static player_ui_control_ops_t s_control_ops;
static void *s_control_ctx = NULL;
static bool s_shuffle_ui_state = false;
static bool s_shuffle_ui_state_valid = false;
static ytmd_repeat_t s_repeat_ui_state = YTMD_REPEAT_NONE;
static bool s_repeat_ui_state_valid = false;
static bool s_skip_prev_feedback_pending = false;
static bool s_skip_next_feedback_pending = false;
static int s_skip_prev_press_seek_seconds = -1;
static int s_skip_next_press_seek_seconds = -1;
static uint32_t s_skip_prev_press_tick = 0;
static uint32_t s_skip_next_press_tick = 0;
static int s_latest_elapsed_seconds = -1;
static int s_latest_song_duration_seconds = -1;
static bool s_seekbar_touch_active = false;
static int s_seekbar_touch_target_seconds = -1;
static char s_last_track_title[PLAYER_TITLE_MAX_LEN] = {0};
static char s_last_track_artist[PLAYER_ARTIST_MAX_LEN] = {0};
static lv_obj_t *s_album_bg = NULL;
static uint32_t s_seekbar_indicator_color_hex = 0;
static bool s_seekbar_indicator_color_valid = false;

typedef struct {
    const char *name;
    lv_obj_t **ref;
} object_ref_entry_t;

static const object_ref_entry_t s_generated_object_map[] = {
    { "album_art", &objects.album_art },
    { "song_title", &objects.song_title },
    { "song_artist", &objects.song_artist },
    { "play", &objects.play },
    { "skip_prev", &objects.skip_prev },
    { "skip_next", &objects.skip_next },
    { "song_random", &objects.song_random },
    { "song_repeat", &objects.song_repeat },
    { "song_like", &objects.song_like },
    { "song_senti", &objects.song_senti },
    { "next", &objects.next },
    { "next_song", &objects.next_song },
    { "seekbar", &objects.seekbar },
    { "time_now", &objects.time_now },
    { "total_time", &objects.total_time },
};

#if defined(LV_USE_OBJ_NAME) && LV_USE_OBJ_NAME
static lv_obj_t *find_object_in_tree_by_name(lv_obj_t *root, const char *name)
{
    if (!root || !name) {
        return NULL;
    }

    const char *obj_name = lv_obj_get_name(root);
    if (obj_name && strcmp(obj_name, name) == 0) {
        return root;
    }

    const uint32_t child_count = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        lv_obj_t *found = find_object_in_tree_by_name(child, name);
        if (found) {
            return found;
        }
    }

    return NULL;
}
#endif

static lv_obj_t *find_generated_object_by_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return NULL;
    }

    const size_t count = sizeof(s_generated_object_map) / sizeof(s_generated_object_map[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_generated_object_map[i].name, name) == 0) {
            return s_generated_object_map[i].ref ? *(s_generated_object_map[i].ref) : NULL;
        }
    }

    return NULL;
}

static lv_obj_t *resolve_object_by_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return NULL;
    }

#if defined(LV_USE_OBJ_NAME) && LV_USE_OBJ_NAME
    if (objects.main) {
        lv_obj_t *from_tree = find_object_in_tree_by_name(objects.main, name);
        if (from_tree) {
            return from_tree;
        }
    }
#endif

    lv_obj_t *obj = find_generated_object_by_name(name);
    if (!obj) {
        ESP_LOGW(TAG, "UI object not found: %s", name);
    }
    return obj;
}

typedef struct {
    const char *name;
    lv_obj_t **target;
} object_bind_entry_t;

static void bind_ui_objects_by_name(void)
{
    memset(&s_ui, 0, sizeof(s_ui));

    const object_bind_entry_t binds[] = {
        { "album_art", &s_ui.album_art },
        { "song_title", &s_ui.song_title },
        { "song_artist", &s_ui.song_artist },
        { "play", &s_ui.play },
        { "skip_prev", &s_ui.skip_prev },
        { "skip_next", &s_ui.skip_next },
        { "song_random", &s_ui.song_random },
        { "song_repeat", &s_ui.song_repeat },
        { "song_like", &s_ui.song_like },
        { "song_senti", &s_ui.song_senti },
        { "next", &s_ui.next },
        { "next_song", &s_ui.next_song },
        { "seekbar", &s_ui.seekbar },
        { "time_now", &s_ui.time_now },
        { "total_time", &s_ui.total_time },
    };

    const size_t count = sizeof(binds) / sizeof(binds[0]);
    for (size_t i = 0; i < count; ++i) {
        *(binds[i].target) = resolve_object_by_name(binds[i].name);
    }
}

static const lv_img_dsc_t *find_image_by_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return NULL;
    }

    const size_t count = sizeof(images) / sizeof(images[0]);
    for (size_t i = 0; i < count; ++i) {
        if (images[i].name && strcmp(images[i].name, name) == 0) {
            return images[i].img_dsc;
        }
    }

    ESP_LOGW(TAG, "UI image resource not found: %s", name);
    return NULL;
}

static void set_image_by_name(lv_obj_t *image_obj, const char *image_name)
{
    if (!image_obj) {
        ESP_LOGW(TAG, "Target image object is NULL for resource: %s", image_name ? image_name : "(null)");
        return;
    }

    const lv_img_dsc_t *dsc = find_image_by_name(image_name);
    if (!dsc) {
        return;
    }

    lv_img_set_src(image_obj, dsc);
}

static void apply_player_fonts(void)
{
    const lv_font_t *font_title = ui_display_font_large();
    const lv_font_t *font_body = ui_display_font_small();

    if (!font_title) {
        ESP_LOGW(TAG, "Title font is not available (ui_display_font_large returned NULL)");
    }
    if (!font_body) {
        ESP_LOGW(TAG, "Body font is not available (ui_display_font_small returned NULL)");
    }

    if (s_ui.song_title && font_title) {
        lv_obj_set_style_text_font(s_ui.song_title, font_title, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (font_body) {
        if (s_ui.song_artist) {
            lv_obj_set_style_text_font(s_ui.song_artist, font_body, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        if (s_ui.next) {
            lv_obj_set_style_text_font(s_ui.next, font_body, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        if (s_ui.next_song) {
            lv_obj_set_style_text_font(s_ui.next_song, font_body, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        if (s_ui.time_now) {
            lv_obj_set_style_text_font(s_ui.time_now, font_body, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        if (s_ui.total_time) {
            lv_obj_set_style_text_font(s_ui.total_time, font_body, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

static void build_next_song_string(char *out, size_t out_cap, const char *title, const char *artist)
{
    if (!out || out_cap == 0) {
        return;
    }

    out[0] = '\0';

    bool has_title = false;
    bool has_artist = false;

    if (title) {
        for (const char *p = title; *p; ++p) {
            if (!isspace((unsigned char)*p)) {
                has_title = true;
                break;
            }
        }
    }
    if (artist) {
        for (const char *p = artist; *p; ++p) {
            if (!isspace((unsigned char)*p)) {
                has_artist = true;
                break;
            }
        }
    }

    if (!has_title && !has_artist) {
        snprintf(out, out_cap, "-");
        return;
    }

    if (has_title && has_artist) {
        snprintf(out, out_cap, "%s-%s", title, artist);
        return;
    }

    snprintf(out, out_cap, "%s", has_title ? title : artist);
}

static void apply_play_state_image(bool is_playing)
{
    set_image_by_name(s_ui.play, is_playing ? "pause" : "play");
}

static void apply_shuffle_state_image(bool is_shuffle)
{
    set_image_by_name(s_ui.song_random, is_shuffle ? "Shuffle_enable" : "Shuffle_disable");
}

static void apply_repeat_state_image(ytmd_repeat_t repeat)
{
    const char *image_name = "Repeat_disable";

    if (repeat == YTMD_REPEAT_ALL) {
        image_name = "Repeat_ALL_enable";
    } else if (repeat == YTMD_REPEAT_ONE) {
        image_name = "Repeat_One_enable";
    }

    set_image_by_name(s_ui.song_repeat, image_name);
}

static ytmd_repeat_t next_repeat_state(ytmd_repeat_t current)
{
    if (current == YTMD_REPEAT_NONE) {
        return YTMD_REPEAT_ALL;
    }
    if (current == YTMD_REPEAT_ALL) {
        return YTMD_REPEAT_ONE;
    }
    return YTMD_REPEAT_NONE;
}

static void apply_like_state_image(bool is_liked)
{
    set_image_by_name(s_ui.song_like, is_liked ? "Thumb_Up_enable" : "Thumb_Up_disable");
}

static void apply_dislike_state_image(bool is_disliked)
{
    set_image_by_name(s_ui.song_senti, is_disliked ? "Thumb_Down_enable" : "Thumb_Down_disable");
}

static void setup_album_art_background(void)
{
    if (!objects.main) {
        return;
    }

    if (!s_album_bg) {
        s_album_bg = lv_image_create(objects.main);
        if (!s_album_bg) {
            ESP_LOGW(TAG, "Failed to create album background layer");
            return;
        }

        lv_obj_clear_flag(s_album_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_img_opa(s_album_bg, PLAYER_BG_ART_OPA, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor(s_album_bg, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(s_album_bg, PLAYER_BG_ART_RECOLOR_OPA, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    int screen_w = lv_obj_get_width(objects.main);
    int screen_h = lv_obj_get_height(objects.main);
    if (screen_w <= 0) {
        screen_w = 800;
    }
    if (screen_h <= 0) {
        screen_h = 480;
    }

    lv_obj_set_size(s_album_bg, UI_ALBUM_ART_W, UI_ALBUM_ART_H);
    lv_obj_set_pos(s_album_bg,
                   (screen_w - UI_ALBUM_ART_W) / 2,
                   (screen_h - UI_ALBUM_ART_H) / 2);
    lv_image_set_pivot(s_album_bg, UI_ALBUM_ART_W / 2, UI_ALBUM_ART_H / 2);

    uint32_t scale_x = (uint32_t)((screen_w * 256 + UI_ALBUM_ART_W - 1) / UI_ALBUM_ART_W);
    uint32_t scale_y = (uint32_t)((screen_h * 256 + UI_ALBUM_ART_H - 1) / UI_ALBUM_ART_H);
    uint32_t scale = (scale_x > scale_y) ? scale_x : scale_y;
    if (scale < LV_SCALE_NONE) {
        scale = LV_SCALE_NONE;
    }
    lv_image_set_scale(s_album_bg, scale);

    const lv_img_dsc_t *album_dsc = ui_display_get_album_dsc();
    if (album_dsc) {
        lv_img_set_src(s_album_bg, album_dsc);
    }
    lv_obj_move_to_index(s_album_bg, 0);
}

static void update_album_art_background(const lv_img_dsc_t *album_dsc)
{
    if (!s_album_bg || !album_dsc) {
        return;
    }
    lv_img_set_src(s_album_bg, album_dsc);
    lv_obj_invalidate(s_album_bg);
}

static bool has_visible_text(const char *text)
{
    if (!text) {
        return false;
    }

    for (const char *p = text; *p; ++p) {
        if (!isspace((unsigned char)*p)) {
            return true;
        }
    }
    return false;
}

static int resolve_elapsed_seconds(const ytmd_player_state_t *state)
{
    if (!state) {
        return -1;
    }
    if (state->has_elapsed_seconds) {
        return state->elapsed_seconds;
    }
    if (state->has_seek_seconds) {
        return state->seek_seconds;
    }
    return -1;
}

static void format_time_label(int seconds, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }

    if (seconds < 0) {
        snprintf(out, out_cap, "--:--");
        return;
    }

    const int h = seconds / 3600;
    const int m = (seconds % 3600) / 60;
    const int s = seconds % 60;

    if (h > 0) {
        snprintf(out, out_cap, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(out, out_cap, "%d:%02d", m, s);
    }
}

static uint32_t sample_seekbar_indicator_color_hex(const lv_img_dsc_t *album_dsc)
{
    if (!album_dsc || !album_dsc->data || album_dsc->header.w <= 0 || album_dsc->header.h <= 0) {
        return PLAYER_SEEKBAR_FALLBACK_COLOR_HEX;
    }

    const int w = album_dsc->header.w;
    const int h = album_dsc->header.h;
    const uint16_t *pixels = (const uint16_t *)album_dsc->data;
    const int grid = 12;
    uint64_t sum_r = 0;
    uint64_t sum_g = 0;
    uint64_t sum_b = 0;
    int samples = 0;

    for (int yi = 0; yi < grid; ++yi) {
        const int y = ((h - 1) * (yi + 1)) / (grid + 1);
        for (int xi = 0; xi < grid; ++xi) {
            const int x = ((w - 1) * (xi + 1)) / (grid + 1);
            const uint16_t px = pixels[y * w + x];
            int r = ((px >> 11) & 0x1F) * 255 / 31;
            int g = ((px >> 5) & 0x3F) * 255 / 63;
            int b = (px & 0x1F) * 255 / 31;
            const int luma = (r * 30 + g * 59 + b * 11) / 100;
            if (luma < 12) {
                continue;
            }
            sum_r += (uint64_t)r;
            sum_g += (uint64_t)g;
            sum_b += (uint64_t)b;
            samples++;
        }
    }

    if (samples <= 0) {
        return PLAYER_SEEKBAR_FALLBACK_COLOR_HEX;
    }

    int r = (int)(sum_r / (uint64_t)samples);
    int g = (int)(sum_g / (uint64_t)samples);
    int b = (int)(sum_b / (uint64_t)samples);

    // Background layer is intentionally darkened; keep indicator close to it.
    r = (r * 75) / 100;
    g = (g * 75) / 100;
    b = (b * 75) / 100;

    int luma = (r * 30 + g * 59 + b * 11) / 100;
    if (luma < PLAYER_SEEKBAR_MIN_LUMA) {
        const int add = PLAYER_SEEKBAR_MIN_LUMA - luma;
        r += add;
        g += add;
        b += add;
    }

    if (r > 255) {
        r = 255;
    }
    if (g > 255) {
        g = 255;
    }
    if (b > 255) {
        b = 255;
    }

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void apply_seekbar_palette(const lv_img_dsc_t *album_dsc)
{
    if (!s_ui.seekbar) {
        return;
    }

    uint32_t color_hex = PLAYER_SEEKBAR_FALLBACK_COLOR_HEX;
    if (album_dsc) {
        color_hex = sample_seekbar_indicator_color_hex(album_dsc);
    } else if (s_seekbar_indicator_color_valid) {
        color_hex = s_seekbar_indicator_color_hex;
    }

    s_seekbar_indicator_color_hex = color_hex;
    s_seekbar_indicator_color_valid = true;

    lv_obj_set_style_bg_color(s_ui.seekbar, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_ui.seekbar, PLAYER_SEEKBAR_BG_OPA, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_ui.seekbar, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_ui.seekbar, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(s_ui.seekbar, lv_color_hex(color_hex), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_ui.seekbar, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_ui.seekbar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

static void update_playback_timeline(const ytmd_player_state_t *state)
{
    if (!state) {
        return;
    }

    const int elapsed_seconds = resolve_elapsed_seconds(state);
    const int total_seconds = state->has_song_duration_seconds ? state->song_duration_seconds : -1;
    s_latest_song_duration_seconds = total_seconds;

    char now_label[16] = {0};
    char total_label[16] = {0};
    format_time_label(elapsed_seconds, now_label, sizeof(now_label));
    format_time_label(total_seconds, total_label, sizeof(total_label));

    if (s_ui.time_now) {
        lv_label_set_text(s_ui.time_now, now_label);
    }
    if (s_ui.total_time) {
        lv_label_set_text(s_ui.total_time, total_label);
    }

    if (!s_ui.seekbar) {
        return;
    }

    if (total_seconds > 0) {
        int value = elapsed_seconds;
        if (value < 0) {
            value = 0;
        } else if (value > total_seconds) {
            value = total_seconds;
        }
        if (!s_seekbar_touch_active) {
            lv_bar_set_range(s_ui.seekbar, 0, total_seconds);
            lv_bar_set_value(s_ui.seekbar, value, LV_ANIM_OFF);
        }
    } else {
        lv_bar_set_range(s_ui.seekbar, 0, 100);
        lv_bar_set_value(s_ui.seekbar, 0, LV_ANIM_OFF);
    }
}

static int seek_seconds_from_touch_point(lv_obj_t *bar, const lv_point_t *point, int total_seconds)
{
    if (!bar || !point || total_seconds <= 0) {
        return -1;
    }

    lv_area_t coords;
    lv_obj_get_coords(bar, &coords);

    const int width = coords.x2 - coords.x1 + 1;
    if (width <= 1) {
        return -1;
    }

    int rel_x = point->x - coords.x1;
    if (rel_x < 0) {
        rel_x = 0;
    } else if (rel_x > width - 1) {
        rel_x = width - 1;
    }

    int target_seconds = (int)(((int64_t)rel_x * (int64_t)total_seconds + (width - 1) / 2) / (width - 1));
    if (target_seconds < 0) {
        target_seconds = 0;
    } else if (target_seconds > total_seconds) {
        target_seconds = total_seconds;
    }
    return target_seconds;
}

static void apply_seek_timeline_ui(int target_seconds, int total_seconds)
{
    if (target_seconds < 0 || total_seconds <= 0) {
        return;
    }

    if (s_ui.seekbar) {
        lv_bar_set_range(s_ui.seekbar, 0, total_seconds);
        lv_bar_set_value(s_ui.seekbar, target_seconds, LV_ANIM_OFF);
    }

    if (s_ui.time_now) {
        char now_label[16] = {0};
        format_time_label(target_seconds, now_label, sizeof(now_label));
        lv_label_set_text(s_ui.time_now, now_label);
    }
    if (s_ui.total_time) {
        char total_label[16] = {0};
        format_time_label(total_seconds, total_label, sizeof(total_label));
        lv_label_set_text(s_ui.total_time, total_label);
    }

    s_latest_elapsed_seconds = target_seconds;
}

static bool is_track_changed_from_last(const ytmd_player_state_t *state)
{
    if (!state) {
        return false;
    }

    const bool prev_has_track = has_visible_text(s_last_track_title) || has_visible_text(s_last_track_artist);
    const bool cur_has_track = has_visible_text(state->title) || has_visible_text(state->artist);
    if (!prev_has_track || !cur_has_track) {
        return false;
    }

    return (strcmp(s_last_track_title, state->title) != 0) ||
           (strcmp(s_last_track_artist, state->artist) != 0);
}

static void restore_skip_prev_image(void)
{
    set_image_by_name(s_ui.skip_prev, "skip_previous");
    if (s_ui.skip_prev) {
        lv_obj_invalidate(s_ui.skip_prev);
    }
    s_skip_prev_feedback_pending = false;
    s_skip_prev_press_seek_seconds = -1;
    s_skip_prev_press_tick = 0;
}

static void restore_skip_next_image(void)
{
    set_image_by_name(s_ui.skip_next, "skip_next_");
    if (s_ui.skip_next) {
        lv_obj_invalidate(s_ui.skip_next);
    }
    s_skip_next_feedback_pending = false;
    s_skip_next_press_seek_seconds = -1;
    s_skip_next_press_tick = 0;
}

static void begin_skip_prev_feedback(void)
{
    s_skip_prev_feedback_pending = true;
    s_skip_prev_press_seek_seconds = s_latest_elapsed_seconds;
    s_skip_prev_press_tick = lv_tick_get();
    set_image_by_name(s_ui.skip_prev, "skip_previous_push");
    if (s_ui.skip_prev) {
        lv_obj_invalidate(s_ui.skip_prev);
    }
}

static void begin_skip_next_feedback(void)
{
    s_skip_next_feedback_pending = true;
    s_skip_next_press_seek_seconds = s_latest_elapsed_seconds;
    s_skip_next_press_tick = lv_tick_get();
    set_image_by_name(s_ui.skip_next, "skip_next_push");
    if (s_ui.skip_next) {
        lv_obj_invalidate(s_ui.skip_next);
    }
}

static void update_skip_feedback(const ytmd_player_state_t *state)
{
    if (!state) {
        return;
    }

    const bool track_changed = is_track_changed_from_last(state);
    const int elapsed_seconds = resolve_elapsed_seconds(state);
    const uint32_t now = lv_tick_get();

    if (s_skip_next_feedback_pending) {
        bool done = false;
        if (track_changed) {
            done = true;
        } else if (s_skip_next_press_seek_seconds >= 0 &&
                   elapsed_seconds >= 0 &&
                   elapsed_seconds + 2 < s_skip_next_press_seek_seconds) {
            done = true;
        } else if ((now - s_skip_next_press_tick) >= PLAYER_SKIP_PUSH_TIMEOUT_MS) {
            done = true;
        }
        if (done) {
            restore_skip_next_image();
        }
    }

    if (s_skip_prev_feedback_pending) {
        bool done = false;
        if (track_changed) {
            done = true;
        } else if (s_skip_prev_press_seek_seconds >= 0 && elapsed_seconds >= 0) {
            const bool rewound_to_head = (elapsed_seconds <= 1) &&
                                         (elapsed_seconds + 2 < s_skip_prev_press_seek_seconds);
            if (s_skip_prev_press_seek_seconds >= PLAYER_PREV_REWIND_THRESHOLD_SEC) {
                done = rewound_to_head;
            } else if (rewound_to_head) {
                done = true;
            }
        }

        if (!done && (now - s_skip_prev_press_tick) >= PLAYER_SKIP_PUSH_TIMEOUT_MS) {
            done = true;
        }
        if (done) {
            restore_skip_prev_image();
        }
    }
}

static void commit_track_snapshot(const ytmd_player_state_t *state)
{
    if (!state) {
        return;
    }

    snprintf(s_last_track_title, sizeof(s_last_track_title), "%s", state->title);
    snprintf(s_last_track_artist, sizeof(s_last_track_artist), "%s", state->artist);
    s_latest_elapsed_seconds = resolve_elapsed_seconds(state);
}

static void setup_album_art_round_mask(void)
{
    if (!s_ui.album_art) {
        return;
    }
    lv_obj_set_style_radius(s_ui.album_art, PLAYER_ALBUM_ART_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(s_ui.album_art, true, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void dispatch_control(void (*registered_cb)(void *), void (*fallback_cb)(void))
{
    if (registered_cb) {
        registered_cb(s_control_ctx);
        return;
    }

    if (fallback_cb) {
        fallback_cb();
    }
}

void player_ui_control_play_pause(void)
{
    dispatch_control(s_control_ops.play_pause, ytmd_cmd_play_pause);
}

void player_ui_control_prev(void)
{
    dispatch_control(s_control_ops.prev, ytmd_cmd_prev);
}

void player_ui_control_next(void)
{
    dispatch_control(s_control_ops.next, ytmd_cmd_next);
}

void player_ui_control_toggle_shuffle(void)
{
    dispatch_control(s_control_ops.toggle_shuffle, ytmd_cmd_toggle_shuffle);

    // Optimistic UI update: shuffle state endpoint may lag behind command response.
    if (!s_shuffle_ui_state_valid) {
        s_shuffle_ui_state = false;
        s_shuffle_ui_state_valid = true;
    }
    s_shuffle_ui_state = !s_shuffle_ui_state;

    if (bsp_display_lock(0)) {
        apply_shuffle_state_image(s_shuffle_ui_state);
        if (s_ui.song_random) {
            lv_obj_invalidate(s_ui.song_random);
        }
        bsp_display_unlock();
    }
}

void player_ui_control_cycle_repeat(void)
{
    dispatch_control(s_control_ops.cycle_repeat, ytmd_cmd_cycle_repeat);

    // Optimistic UI update: /song response may not immediately include repeat state.
    if (!s_repeat_ui_state_valid) {
        s_repeat_ui_state = YTMD_REPEAT_NONE;
        s_repeat_ui_state_valid = true;
    }
    s_repeat_ui_state = next_repeat_state(s_repeat_ui_state);

    if (bsp_display_lock(0)) {
        apply_repeat_state_image(s_repeat_ui_state);
        if (s_ui.song_repeat) {
            lv_obj_invalidate(s_ui.song_repeat);
        }
        bsp_display_unlock();
    }
}

void player_ui_control_toggle_like(void)
{
    dispatch_control(s_control_ops.toggle_like, ytmd_cmd_toggle_like);
}

void player_ui_control_toggle_dislike(void)
{
    dispatch_control(s_control_ops.toggle_dislike, ytmd_cmd_toggle_dislike);
}

static void on_play_clicked(lv_event_t *e)
{
    (void)e;
    player_ui_control_play_pause();
}

static void on_skip_prev_clicked(lv_event_t *e)
{
    (void)e;
    begin_skip_prev_feedback();
    player_ui_control_prev();
}

static void on_skip_next_clicked(lv_event_t *e)
{
    (void)e;
    begin_skip_next_feedback();
    player_ui_control_next();
}

static void on_shuffle_clicked(lv_event_t *e)
{
    (void)e;
    player_ui_control_toggle_shuffle();
}

static void on_repeat_clicked(lv_event_t *e)
{
    (void)e;
    player_ui_control_cycle_repeat();
}

static void on_like_clicked(lv_event_t *e)
{
    (void)e;
    player_ui_control_toggle_like();
}

static void on_senti_clicked(lv_event_t *e)
{
    (void)e;
    player_ui_control_toggle_dislike();
}

static bool get_seekbar_event_point(lv_event_t *e, lv_point_t *out_point)
{
    if (!e || !out_point) {
        return false;
    }

    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) {
        return false;
    }

    lv_indev_get_point(indev, out_point);
    return true;
}

static void on_seekbar_interaction(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED &&
        code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST) {
        return;
    }

    lv_obj_t *bar = lv_event_get_target(e);
    if (!bar) {
        return;
    }
    if (s_latest_song_duration_seconds <= 0) {
        return;
    }

    lv_point_t point = {0};
    if (!get_seekbar_event_point(e, &point)) {
        ESP_LOGW(TAG, "SEEK: input device is NULL");
        return;
    }

    const int target_seconds = seek_seconds_from_touch_point(bar, &point, s_latest_song_duration_seconds);
    if (target_seconds < 0) {
        return;
    }

    s_seekbar_touch_target_seconds = target_seconds;
    apply_seek_timeline_ui(target_seconds, s_latest_song_duration_seconds);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        s_seekbar_touch_active = true;
        return;
    }

    s_seekbar_touch_active = false;
    if (s_seekbar_touch_target_seconds >= 0) {
        ytmd_cmd_seek_to(s_seekbar_touch_target_seconds);
        s_seekbar_touch_target_seconds = -1;
    }
}

static void bind_click_event(lv_obj_t *obj, lv_event_cb_t event_cb, const char *obj_name)
{
    if (!obj) {
        ESP_LOGW(TAG, "Cannot bind event: object is NULL (%s)", obj_name ? obj_name : "(null)");
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, event_cb, LV_EVENT_CLICKED, NULL);
}

static void bind_player_events(void)
{
    bind_click_event(s_ui.play, on_play_clicked, "play");
    bind_click_event(s_ui.skip_prev, on_skip_prev_clicked, "skip_prev");
    bind_click_event(s_ui.skip_next, on_skip_next_clicked, "skip_next");
    bind_click_event(s_ui.song_random, on_shuffle_clicked, "song_random");
    bind_click_event(s_ui.song_repeat, on_repeat_clicked, "song_repeat");
    bind_click_event(s_ui.song_like, on_like_clicked, "song_like");
    bind_click_event(s_ui.song_senti, on_senti_clicked, "song_senti");
    if (s_ui.seekbar) {
        lv_obj_add_flag(s_ui.seekbar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s_ui.seekbar, 16);
        lv_obj_add_event_cb(s_ui.seekbar, on_seekbar_interaction, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_ui.seekbar, on_seekbar_interaction, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(s_ui.seekbar, on_seekbar_interaction, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_ui.seekbar, on_seekbar_interaction, LV_EVENT_PRESS_LOST, NULL);
    } else {
        ESP_LOGW(TAG, "Cannot bind event: object is NULL (seekbar)");
    }
}

void player_ui_set_control_ops(const player_ui_control_ops_t *ops, void *user_ctx)
{
    if (ops) {
        s_control_ops = *ops;
    } else {
        memset(&s_control_ops, 0, sizeof(s_control_ops));
    }
    s_control_ctx = user_ctx;
}

void player_ui_init(void)
{
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Display lock failed during player_ui_init");
        return;
    }

    ui_init();

    bind_ui_objects_by_name();
    setup_album_art_background();
    setup_album_art_round_mask();
    apply_player_fonts();
    apply_seekbar_palette(ui_display_get_album_dsc());
    bind_player_events();

    bsp_display_unlock();

    ytmd_player_state_t initial_state = {0};
    player_ui_update(&initial_state);
    player_ui_update_album_art();

    ESP_LOGI(TAG, "Player UI initialized");
}

void player_ui_update_album_art(void)
{
    if (!s_ui.album_art) {
        ESP_LOGW(TAG, "album_art object is NULL");
        return;
    }

    const lv_img_dsc_t *album_dsc = ui_display_get_album_dsc();
    if (!album_dsc) {
        ESP_LOGW(TAG, "Album art descriptor is NULL");
        return;
    }

    if (!bsp_display_lock(0)) {
        ESP_LOGW(TAG, "Display lock failed during album art update");
        return;
    }

    lv_img_set_src(s_ui.album_art, album_dsc);
    lv_obj_invalidate(s_ui.album_art);
    update_album_art_background(album_dsc);
    apply_seekbar_palette(album_dsc);

    bsp_display_unlock();
}

void player_ui_update(const ytmd_player_state_t *state)
{
    if (!state) {
        ESP_LOGW(TAG, "player_ui_update called with NULL state");
        return;
    }

    if (!bsp_display_lock(0)) {
        ESP_LOGW(TAG, "Display lock failed during player_ui_update");
        return;
    }

    /* UI 재생성/스타일 덮어쓰기 상황에서도 폰트가 유지되도록 갱신 시 재적용 */
    apply_player_fonts();

    if (s_ui.song_title) {
        lv_label_set_text(s_ui.song_title, (state->title[0] != '\0') ? state->title : "-");
    }

    if (s_ui.song_artist) {
        lv_label_set_text(s_ui.song_artist, (state->artist[0] != '\0') ? state->artist : "-");
    }

    apply_play_state_image(state->is_playing);
    s_shuffle_ui_state = state->is_shuffle;
    s_shuffle_ui_state_valid = true;
    apply_shuffle_state_image(state->is_shuffle);
    s_repeat_ui_state = state->repeat;
    s_repeat_ui_state_valid = true;
    apply_repeat_state_image(state->repeat);
    apply_like_state_image(state->is_liked);
    apply_dislike_state_image(state->is_disliked);
    update_skip_feedback(state);

    if (s_ui.next) {
        lv_label_set_text(s_ui.next, PLAYER_NEXT_LABEL_TEXT);
    }

    if (s_ui.next_song) {
        char next_line[PLAYER_TITLE_MAX_LEN + PLAYER_ARTIST_MAX_LEN + 4] = {0};
        build_next_song_string(next_line, sizeof(next_line), state->next_title, state->next_artist);
        lv_label_set_text(s_ui.next_song, next_line);
    }
    update_playback_timeline(state);

    if (s_ui.album_art && state->album_art_dsc) {
        lv_img_set_src(s_ui.album_art, state->album_art_dsc);
    }
    if (state->album_art_dsc) {
        update_album_art_background(state->album_art_dsc);
        apply_seekbar_palette(state->album_art_dsc);
    }

    commit_track_snapshot(state);

    bsp_display_unlock();
}

void ytmd_cmd_play_pause(void)
{
    esp_err_t err = ytmd_client_cmd_play_pause();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(play/pause): sent");
    } else {
        ESP_LOGW(TAG, "CMD(play/pause) failed: %s", esp_err_to_name(err));
    }
}

void ytmd_cmd_next(void)
{
    esp_err_t err = ytmd_client_cmd_next();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(next): sent");
    } else {
        ESP_LOGW(TAG, "CMD(next) failed: %s", esp_err_to_name(err));
    }
}

void ytmd_cmd_prev(void)
{
    esp_err_t err = ytmd_client_cmd_prev();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(prev): sent");
    } else {
        ESP_LOGW(TAG, "CMD(prev) failed: %s", esp_err_to_name(err));
    }
}

void ytmd_cmd_seek_to(int seconds)
{
    esp_err_t err = ytmd_client_cmd_seek_to(seconds);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(seek-to): %d sec", seconds);
    } else {
        ESP_LOGW(TAG, "CMD(seek-to=%d) failed: %s", seconds, esp_err_to_name(err));
    }
}

void ytmd_cmd_toggle_shuffle(void)
{
    esp_err_t err = ytmd_client_cmd_toggle_shuffle();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(shuffle): sent");
    } else {
        ESP_LOGW(TAG, "CMD(shuffle) failed: %s", esp_err_to_name(err));
    }
}

void ytmd_cmd_cycle_repeat(void)
{
    esp_err_t err = ytmd_client_cmd_cycle_repeat();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(repeat): sent");
    } else {
        ESP_LOGW(TAG, "CMD(repeat) failed: %s", esp_err_to_name(err));
    }
}

void ytmd_cmd_toggle_like(void)
{
    esp_err_t err = ytmd_client_cmd_toggle_like();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(like): sent");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "CMD(like): endpoint not supported yet");
    } else {
        ESP_LOGW(TAG, "CMD(like) failed: %s", esp_err_to_name(err));
    }
}

void ytmd_cmd_toggle_dislike(void)
{
    esp_err_t err = ytmd_client_cmd_toggle_dislike();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(dislike): sent");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "CMD(dislike): endpoint not supported yet");
    } else {
        ESP_LOGW(TAG, "CMD(dislike) failed: %s", esp_err_to_name(err));
    }
}
