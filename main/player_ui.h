#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_TITLE_MAX_LEN   256
#define PLAYER_ARTIST_MAX_LEN  128

typedef enum {
    YTMD_REPEAT_NONE = 0,
    YTMD_REPEAT_ALL,
    YTMD_REPEAT_ONE,
} ytmd_repeat_t;

typedef struct {
    char title[PLAYER_TITLE_MAX_LEN];
    char artist[PLAYER_ARTIST_MAX_LEN];

    const lv_img_dsc_t *album_art_dsc;

    bool is_playing;
    bool is_shuffle;
    ytmd_repeat_t repeat;
    bool is_liked;
    bool is_disliked;

    char next_title[PLAYER_TITLE_MAX_LEN];
    char next_artist[PLAYER_ARTIST_MAX_LEN];

    bool has_seek_seconds;
    int seek_seconds;

    bool has_elapsed_seconds;
    int elapsed_seconds;

    bool has_song_duration_seconds;
    int song_duration_seconds;
} ytmd_player_state_t;

typedef struct {
    void (*play_pause)(void *user_ctx);
    void (*prev)(void *user_ctx);
    void (*next)(void *user_ctx);
    void (*toggle_shuffle)(void *user_ctx);
    void (*cycle_repeat)(void *user_ctx);
    void (*toggle_like)(void *user_ctx);
    void (*toggle_dislike)(void *user_ctx);
} player_ui_control_ops_t;

void player_ui_init(void);
void player_ui_set_control_ops(const player_ui_control_ops_t *ops, void *user_ctx);

void player_ui_update(const ytmd_player_state_t *state);
void player_ui_update_album_art(void);

void player_ui_control_play_pause(void);
void player_ui_control_prev(void);
void player_ui_control_next(void);
void player_ui_control_toggle_shuffle(void);
void player_ui_control_cycle_repeat(void);
void player_ui_control_toggle_like(void);
void player_ui_control_toggle_dislike(void);

void ytmd_cmd_play_pause(void);
void ytmd_cmd_next(void);
void ytmd_cmd_prev(void);
void ytmd_cmd_toggle_shuffle(void);
void ytmd_cmd_cycle_repeat(void);
void ytmd_cmd_toggle_like(void);
void ytmd_cmd_toggle_dislike(void);

#ifdef __cplusplus
}
#endif
