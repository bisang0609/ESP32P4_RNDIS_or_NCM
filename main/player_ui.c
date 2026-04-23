#include "player_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "bsp/esp-bsp.h"

#include "ui/images.h"
#include "ui/screens.h"
#include "ui/ui.h"

#include "ui_display.h"
#include "ytmd_client.h"

static const char *TAG = "PLAYER_UI";

#define PLAYER_NEXT_LABEL_TEXT "Next"

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
} player_ui_objects_t;

static player_ui_objects_t s_ui;
static player_ui_control_ops_t s_control_ops;
static void *s_control_ctx = NULL;

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
    const lv_font_t *font26 = ui_display_font_large();
    const lv_font_t *font16 = ui_display_font_small();

    if (!font26) {
        ESP_LOGW(TAG, "Font 26 is not available (ui_display_font_large returned NULL)");
    }
    if (!font16) {
        ESP_LOGW(TAG, "Font 16 is not available (ui_display_font_small returned NULL)");
    }

    if (s_ui.song_title && font26) {
        lv_obj_set_style_text_font(s_ui.song_title, font26, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (font16) {
        if (s_ui.song_artist) {
            lv_obj_set_style_text_font(s_ui.song_artist, font16, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        if (s_ui.next) {
            lv_obj_set_style_text_font(s_ui.next, font16, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        if (s_ui.next_song) {
            lv_obj_set_style_text_font(s_ui.next_song, font16, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

static void build_next_song_string(char *out, size_t out_cap, const char *title, const char *artist)
{
    if (!out || out_cap == 0) {
        return;
    }

    out[0] = '\0';

    const bool has_title = (title && title[0] != '\0');
    const bool has_artist = (artist && artist[0] != '\0');

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
    set_image_by_name(s_ui.song_random, is_shuffle ? "shu_enable" : "shu_disenable");
}

static void apply_repeat_state_image(ytmd_repeat_t repeat)
{
    const char *image_name = "repeat_disable";

    if (repeat == YTMD_REPEAT_ALL) {
        image_name = "repeat_all";
    } else if (repeat == YTMD_REPEAT_ONE) {
        image_name = "repeat_one";
    }

    set_image_by_name(s_ui.song_repeat, image_name);
}

static void apply_like_state_image(bool is_liked)
{
    set_image_by_name(s_ui.song_like, is_liked ? "like_enable" : "like_disenable");
}

static void apply_dislike_state_image(bool is_disliked)
{
    set_image_by_name(s_ui.song_senti, is_disliked ? "Sentiment Neutral_enable" : "Sentiment Neutral_disable");
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
}

void player_ui_control_cycle_repeat(void)
{
    dispatch_control(s_control_ops.cycle_repeat, ytmd_cmd_cycle_repeat);
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
    player_ui_control_prev();
}

static void on_skip_next_clicked(lv_event_t *e)
{
    (void)e;
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
    apply_player_fonts();
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
    apply_shuffle_state_image(state->is_shuffle);
    apply_repeat_state_image(state->repeat);
    apply_like_state_image(state->is_liked);
    apply_dislike_state_image(state->is_disliked);

    if (s_ui.next) {
        lv_label_set_text(s_ui.next, PLAYER_NEXT_LABEL_TEXT);
    }

    if (s_ui.next_song) {
        char next_line[PLAYER_TITLE_MAX_LEN + PLAYER_ARTIST_MAX_LEN + 4] = {0};
        build_next_song_string(next_line, sizeof(next_line), state->next_title, state->next_artist);
        lv_label_set_text(s_ui.next_song, next_line);
    }

    if (s_ui.album_art && state->album_art_dsc) {
        lv_img_set_src(s_ui.album_art, state->album_art_dsc);
    }

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
