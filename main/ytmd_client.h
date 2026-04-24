#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define YTMD_ART_URL_MAX_LEN 1024
#define YTMD_POLL_INTERVAL_MS 500

typedef enum {
    YTMD_CLIENT_REPEAT_NONE = 0,
    YTMD_CLIENT_REPEAT_ALL,
    YTMD_CLIENT_REPEAT_ONE,
} ytmd_client_repeat_t;

typedef struct {
    bool has_playing;
    bool is_playing;
    bool has_paused;
    bool is_paused;

    bool has_shuffle;
    bool is_shuffle;

    bool has_repeat;
    ytmd_client_repeat_t repeat;

    bool has_liked;
    bool is_liked;

    bool has_disliked;
    bool is_disliked;

    bool has_next_song;
    char next_title[256];
    char next_artist[128];

    bool has_seek_seconds;
    int seek_seconds;

    bool has_elapsed_seconds;
    int elapsed_seconds;

    bool has_song_duration_seconds;
    int song_duration_seconds;
} ytmd_client_playback_state_t;

typedef struct {
    int index;
    bool selected;
    char title[256];
    char artist[128];
    char video_id[32];
    char art_url[YTMD_ART_URL_MAX_LEN];
} ytmd_client_queue_item_t;

typedef struct {
    int index;
    bool selected;
    char title[256];
    char artist[128];
    char video_id[32];
} ytmd_client_queue_compact_item_t;

typedef void (*ytmd_network_diag_cb_t)(const char *url);

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
                                      ytmd_network_diag_cb_t net_diag_cb);

esp_err_t ytmd_client_cmd_play_pause(void);
esp_err_t ytmd_client_cmd_prev(void);
esp_err_t ytmd_client_cmd_next(void);
esp_err_t ytmd_client_cmd_seek_to(int seconds);
esp_err_t ytmd_client_cmd_toggle_shuffle(void);
esp_err_t ytmd_client_cmd_cycle_repeat(void);
esp_err_t ytmd_client_cmd_toggle_like(void);
esp_err_t ytmd_client_cmd_toggle_dislike(void);
esp_err_t ytmd_client_cmd_play_queue_index(int index);
esp_err_t ytmd_client_cmd_play_video_id(const char *video_id);

esp_err_t ytmd_client_refresh_queue_cache(ytmd_network_diag_cb_t net_diag_cb);

esp_err_t ytmd_client_queue_cache_get(ytmd_client_queue_item_t *out_items,
                                      size_t max_items,
                                      size_t *out_copied,
                                      size_t *out_total,
                                      int *out_selected_pos);
esp_err_t ytmd_client_queue_cache_get_compact(ytmd_client_queue_compact_item_t *out_items,
                                              size_t max_items,
                                              size_t *out_copied,
                                              size_t *out_total,
                                              int *out_selected_pos);

esp_err_t ytmd_client_try_get_cached_art_by_queue_offset(int rel_offset,
                                                         uint16_t *dst_rgb565,
                                                         int dst_w,
                                                         int dst_h,
                                                         char *out_art_key,
                                                         size_t out_art_key_cap,
                                                         char *out_title,
                                                         size_t out_title_cap,
                                                         char *out_artist,
                                                         size_t out_artist_cap);

esp_err_t ytmd_client_enrich_playback_state(ytmd_client_playback_state_t *state,
                                            ytmd_network_diag_cb_t net_diag_cb);
