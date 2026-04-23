#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define YTMD_ART_URL_MAX_LEN 1024
#define YTMD_POLL_INTERVAL_MS 2000

typedef enum {
    YTMD_CLIENT_REPEAT_NONE = 0,
    YTMD_CLIENT_REPEAT_ALL,
    YTMD_CLIENT_REPEAT_ONE,
} ytmd_client_repeat_t;

typedef struct {
    bool has_playing;
    bool is_playing;

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
} ytmd_client_playback_state_t;

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
esp_err_t ytmd_client_cmd_toggle_shuffle(void);
esp_err_t ytmd_client_cmd_cycle_repeat(void);
esp_err_t ytmd_client_cmd_toggle_like(void);
esp_err_t ytmd_client_cmd_toggle_dislike(void);
