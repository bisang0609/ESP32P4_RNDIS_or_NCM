/*
 * USB-NCM YTMD album art viewer for ESP32-P4.
 *
 * High-level flow:
 * 1) Bring up USB-NCM network link to host
 * 2) Poll YTMD /api/v1/song
 * 3) Download/prepare album art
 * 4) Render album art on LVGL UI
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "soc/soc_caps.h"

#include "ncm_net.h"
#include "player_ui.h"
#include "ui_display.h"
#include "ytmd_client.h"

static const char *TAG = "USB_NCM_YTMD_ART";
#define YTMD_AUX_STATE_ENRICH_INTERVAL_MS 1500
#define YTMD_QUEUE_REFRESH_INTERVAL_MS 2500
#define YTMD_PREV_SEEK_SETTLE_MS 80
#define YTMD_PREV_TRANSITION_GUARD_MS 1200
#define YTMD_TRACK_SWITCH_PREP_TIMEOUT_MS 5000

static char s_last_art_url[YTMD_ART_URL_MAX_LEN] = {0};
static TaskHandle_t s_album_task_handle = NULL;
static uint32_t s_prev_transition_guard_until_tick = 0;
static volatile bool s_pending_track_switch = false;
static volatile int s_pending_track_switch_dir = 0; // -1: prev, +1: next
static volatile bool s_pending_resume_after_ready = false;
static volatile uint32_t s_pending_track_switch_deadline_tick = 0;
static char s_pending_from_title[PLAYER_TITLE_MAX_LEN] = {0};
static char s_pending_from_artist[PLAYER_ARTIST_MAX_LEN] = {0};

/* ?¬ìƒ ?íƒœ ??ytmd_client ?•ìž¥ ????êµ¬ì¡°ì²´ë? ì±„ì›Œ player_ui_update() ???„ë‹¬ */
static ytmd_player_state_t s_player_state = {0};
static bool update_text_field(char *dst, size_t dst_cap, const char *src);
static void prefer_queue_json_title_artist(char *io_title, size_t title_cap, char *io_artist, size_t artist_cap);
static esp_err_t __attribute__((unused)) apply_cached_art_from_queue_offset(int rel_offset, const char *reason)
{
    char cached_art_key[YTMD_ART_URL_MAX_LEN] = {0};
    char cached_title[PLAYER_TITLE_MAX_LEN] = {0};
    char cached_artist[PLAYER_ARTIST_MAX_LEN] = {0};
    esp_err_t err = ytmd_client_try_get_cached_art_by_queue_offset(
        rel_offset,
        ui_display_get_album_buffer(),
        UI_ALBUM_ART_W,
        UI_ALBUM_ART_H,
        cached_art_key,
        sizeof(cached_art_key),
        cached_title,
        sizeof(cached_title),
        cached_artist,
        sizeof(cached_artist));
    if (err != ESP_OK) {
        return err;
    }
    bool text_changed = false;
    text_changed |= update_text_field(s_player_state.title, sizeof(s_player_state.title), cached_title);
    text_changed |= update_text_field(s_player_state.artist, sizeof(s_player_state.artist), cached_artist);
    ui_display_present_album_art();
    player_ui_update_album_art();
    if (text_changed) {
        player_ui_update(&s_player_state);
    }
    ESP_LOGI(TAG, "Optimistic album art (%s): %s", reason ? reason : "queue", cached_art_key);
    return ESP_OK;
}
static void request_album_refresh_immediate(void)
{
    if (s_album_task_handle) {
        xTaskNotifyGive(s_album_task_handle);
    }
}

static void clear_pending_track_switch(void)
{
    s_pending_track_switch = false;
    s_pending_track_switch_dir = 0;
    s_pending_resume_after_ready = false;
    s_pending_track_switch_deadline_tick = 0;
    s_pending_from_title[0] = '\0';
    s_pending_from_artist[0] = '\0';
}

static void maybe_resume_playback_after_switch(const char *reason)
{
    if (!s_pending_resume_after_ready) {
        return;
    }

    if (s_player_state.is_playing) {
        // Backend may auto-resume during track switch; avoid toggling to pause again.
        s_pending_resume_after_ready = false;
        ESP_LOGI(TAG, "CMD(play/resume:%s): skipped (already playing)", reason ? reason : "ready");
        return;
    }

    esp_err_t err = ytmd_client_cmd_play_pause();
    if (err == ESP_OK) {
        s_player_state.is_playing = true;
        ESP_LOGI(TAG, "CMD(play/resume:%s): sent", reason ? reason : "ready");
    } else {
        ESP_LOGW(TAG, "CMD(play/resume:%s) failed: %s", reason ? reason : "ready", esp_err_to_name(err));
    }
    s_pending_resume_after_ready = false;
}
static void begin_pending_track_switch(int dir, bool resume_after_ready)
{
    s_pending_track_switch = true;
    s_pending_track_switch_dir = dir;
    s_pending_resume_after_ready = resume_after_ready || s_pending_resume_after_ready;
    s_pending_track_switch_deadline_tick =
        (uint32_t)xTaskGetTickCount() + pdMS_TO_TICKS(YTMD_TRACK_SWITCH_PREP_TIMEOUT_MS);
    snprintf(s_pending_from_title, sizeof(s_pending_from_title), "%s", s_player_state.title);
    snprintf(s_pending_from_artist, sizeof(s_pending_from_artist), "%s", s_player_state.artist);
    // Force next fetch to treat incoming art as fresh during transition.
    s_last_art_url[0] = '\0';
}
static void ui_cmd_prev(void *user_ctx)
{
    (void)user_ctx;

    bool paused_by_us = false;
    if (s_player_state.is_playing) {
        esp_err_t pause_err = ytmd_client_cmd_play_pause();
        if (pause_err == ESP_OK) {
            paused_by_us = true;
            s_player_state.is_playing = false;
            ESP_LOGI(TAG, "CMD(pause before prev): sent");
        } else {
            ESP_LOGW(TAG, "CMD(pause before prev) failed: %s", esp_err_to_name(pause_err));
        }
    }

    // Make previous behavior deterministic: force head position first.
    esp_err_t seek_err = ytmd_client_cmd_seek_to(0);
    if (seek_err != ESP_OK) {
        ESP_LOGW(TAG, "CMD(seek-to=0) before prev failed: %s", esp_err_to_name(seek_err));
    } else {
        vTaskDelay(pdMS_TO_TICKS(YTMD_PREV_SEEK_SETTLE_MS));
    }

    esp_err_t err = ytmd_client_cmd_prev();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(prev): sent");
        s_prev_transition_guard_until_tick =
            (uint32_t)xTaskGetTickCount() + pdMS_TO_TICKS(YTMD_PREV_TRANSITION_GUARD_MS);
        begin_pending_track_switch(-1, paused_by_us);
        request_album_refresh_immediate();
    } else {
        if (paused_by_us) {
            s_pending_resume_after_ready = true;
            maybe_resume_playback_after_switch("prev_fail");
        }
        ESP_LOGW(TAG, "CMD(prev) failed: %s", esp_err_to_name(err));
    }
}

static void ui_cmd_next(void *user_ctx)
{
    (void)user_ctx;

    bool paused_by_us = false;
    if (s_player_state.is_playing) {
        esp_err_t pause_err = ytmd_client_cmd_play_pause();
        if (pause_err == ESP_OK) {
            paused_by_us = true;
            s_player_state.is_playing = false;
            ESP_LOGI(TAG, "CMD(pause before next): sent");
        } else {
            ESP_LOGW(TAG, "CMD(pause before next) failed: %s", esp_err_to_name(pause_err));
        }
    }

    esp_err_t err = ytmd_client_cmd_next();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CMD(next): sent");
        begin_pending_track_switch(+1, paused_by_us);
        request_album_refresh_immediate();
    } else {
        if (paused_by_us) {
            s_pending_resume_after_ready = true;
            maybe_resume_playback_after_switch("next_fail");
        }
        ESP_LOGW(TAG, "CMD(next) failed: %s", esp_err_to_name(err));
    }
}
static bool is_same_track_signature(const char *title_a,
                                    const char *artist_a,
                                    const char *title_b,
                                    const char *artist_b)
{
    const char *ta = title_a ? title_a : "";
    const char *aa = artist_a ? artist_a : "";
    const char *tb = title_b ? title_b : "";
    const char *ab = artist_b ? artist_b : "";
    return (strncmp(ta, tb, PLAYER_TITLE_MAX_LEN - 1) == 0) &&
           (strncmp(aa, ab, PLAYER_ARTIST_MAX_LEN - 1) == 0);
}

static bool update_text_field(char *dst, size_t dst_cap, const char *src)
{
    if (!dst || dst_cap == 0 || !src || src[0] == '\0') {
        return false;
    }
    if (strncmp(dst, src, dst_cap - 1) == 0) {
        return false;
    }
    snprintf(dst, dst_cap, "%s", src);
    return true;
}


static void prefer_queue_json_title_artist(char *io_title, size_t title_cap, char *io_artist, size_t artist_cap)
{
    if (!io_title || title_cap == 0 || !io_artist || artist_cap == 0) {
        return;
    }

    ytmd_client_queue_compact_item_t selected = {0};
    esp_err_t err = ytmd_client_queue_cache_get_selected_compact(&selected);
    if (err != ESP_OK) {
        return;
    }

    if (selected.title[0] != '\0') {
        snprintf(io_title, title_cap, "%s", selected.title);
    }
    if (selected.artist[0] != '\0') {
        snprintf(io_artist, artist_cap, "%s", selected.artist);
    }
}

static bool apply_playback_state_to_player(const ytmd_client_playback_state_t *playback)
{
    if (!playback) {
        return false;
    }

    bool changed = false;

    bool has_play_state = false;
    bool new_is_playing = s_player_state.is_playing;
    if (playback->has_playing) {
        has_play_state = true;
        new_is_playing = playback->is_playing;
    }
    if (playback->has_paused) {
        has_play_state = true;
        new_is_playing = !playback->is_paused;
    }
    if (has_play_state && s_player_state.is_playing != new_is_playing) {
        s_player_state.is_playing = new_is_playing;
        changed = true;
    }
    if (playback->has_shuffle && s_player_state.is_shuffle != playback->is_shuffle) {
        s_player_state.is_shuffle = playback->is_shuffle;
        changed = true;
    }
    if (playback->has_repeat) {
        ytmd_repeat_t mapped = YTMD_REPEAT_NONE;
        if (playback->repeat == YTMD_CLIENT_REPEAT_ALL) {
            mapped = YTMD_REPEAT_ALL;
        } else if (playback->repeat == YTMD_CLIENT_REPEAT_ONE) {
            mapped = YTMD_REPEAT_ONE;
        }
        if (s_player_state.repeat != mapped) {
            s_player_state.repeat = mapped;
            changed = true;
        }
    }
    if (playback->has_liked && s_player_state.is_liked != playback->is_liked) {
        s_player_state.is_liked = playback->is_liked;
        changed = true;
    }
    if (playback->has_disliked && s_player_state.is_disliked != playback->is_disliked) {
        s_player_state.is_disliked = playback->is_disliked;
        changed = true;
    }
    if (playback->has_next_song) {
        changed |= update_text_field(s_player_state.next_title, sizeof(s_player_state.next_title), playback->next_title);
        changed |= update_text_field(s_player_state.next_artist, sizeof(s_player_state.next_artist), playback->next_artist);
    }
    if (playback->has_seek_seconds) {
        if (!s_player_state.has_seek_seconds || s_player_state.seek_seconds != playback->seek_seconds) {
            changed = true;
        }
        s_player_state.has_seek_seconds = true;
        s_player_state.seek_seconds = playback->seek_seconds;
    }
    if (playback->has_elapsed_seconds) {
        if (!s_player_state.has_elapsed_seconds || s_player_state.elapsed_seconds != playback->elapsed_seconds) {
            changed = true;
        }
        s_player_state.has_elapsed_seconds = true;
        s_player_state.elapsed_seconds = playback->elapsed_seconds;
    }
    if (playback->has_song_duration_seconds) {
        if (!s_player_state.has_song_duration_seconds || s_player_state.song_duration_seconds != playback->song_duration_seconds) {
            changed = true;
        }
        s_player_state.has_song_duration_seconds = true;
        s_player_state.song_duration_seconds = playback->song_duration_seconds;
    }

    return changed;
}

static void format_mb_decimal(uint64_t bytes, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    uint64_t milli_mb = (bytes * 1000ULL + 500000ULL) / 1000000ULL;
    snprintf(out, out_len, "%" PRIu64 ".%03" PRIu64, milli_mb / 1000ULL, milli_mb % 1000ULL);
}

static void format_pct_x100(uint64_t used, uint64_t total, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (total == 0) {
        snprintf(out, out_len, "0.00");
        return;
    }
    uint64_t pct_x100 = (used * 10000ULL + total / 2ULL) / total;
    snprintf(out, out_len, "%" PRIu64 ".%02" PRIu64, pct_x100 / 100ULL, pct_x100 % 100ULL);
}

static void log_capacity_report_line(const char *name, uint64_t total, uint64_t used, uint64_t free_bytes)
{
    char total_mb[24] = {0};
    char used_mb[24] = {0};
    char free_mb[24] = {0};
    char used_pct[16] = {0};

    format_mb_decimal(total, total_mb, sizeof(total_mb));
    format_mb_decimal(used, used_mb, sizeof(used_mb));
    format_mb_decimal(free_bytes, free_mb, sizeof(free_mb));
    format_pct_x100(used, total, used_pct, sizeof(used_pct));

    ESP_LOGI(TAG, "%s: total=%" PRIu64 "B(%sMB) used=%" PRIu64 "B(%sMB,%s%%) free=%" PRIu64 "B(%sMB)",
             name, total, total_mb, used, used_mb, used_pct, free_bytes, free_mb);
}

static uint64_t get_flash_partition_footprint_end(void)
{
    uint64_t max_end = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p) {
            continue;
        }
        uint64_t end = (uint64_t)p->address + (uint64_t)p->size;
        if (end > max_end) {
            max_end = end;
        }
    }
    return max_end;
}

static uint32_t get_running_app_image_len(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGW(TAG, "MEM: running partition not found");
        return 0;
    }

    esp_partition_pos_t pos = {
        .offset = running->address,
        .size = running->size,
    };
    esp_image_metadata_t md = {0};
    esp_err_t err = esp_image_get_metadata(&pos, &md);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MEM: esp_image_get_metadata failed for running app: %s", esp_err_to_name(err));
        return 0;
    }
    return md.image_len;
}

static void log_memory_capacity_report(void)
{
    uint32_t flash_total_u32 = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_total_u32);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MEM: esp_flash_get_size failed: %s", esp_err_to_name(err));
    } else {
        const uint64_t flash_total = flash_total_u32;
        const uint64_t footprint_end = get_flash_partition_footprint_end();
        const uint64_t mandatory_end = (uint64_t)ESP_PARTITION_TABLE_OFFSET + (uint64_t)ESP_PARTITION_TABLE_SIZE;
        uint64_t flash_used_alloc = (footprint_end > mandatory_end) ? footprint_end : mandatory_end;
        if (flash_used_alloc > flash_total) {
            flash_used_alloc = flash_total;
        }
        uint64_t flash_free_alloc = flash_total - flash_used_alloc;

        uint32_t bootloader_len = 0;
        err = esp_image_verify_bootloader(&bootloader_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MEM: esp_image_verify_bootloader failed: %s", esp_err_to_name(err));
            bootloader_len = 0;
        }
        uint64_t app_image_len = get_running_app_image_len();
        uint64_t flash_used_images = (uint64_t)bootloader_len + (uint64_t)ESP_PARTITION_TABLE_SIZE + app_image_len;
        if (flash_used_images > flash_total) {
            flash_used_images = flash_total;
        }
        uint64_t flash_free_images = flash_total - flash_used_images;

        log_capacity_report_line("FLASH(alloc)", flash_total, flash_used_alloc, flash_free_alloc);
        log_capacity_report_line("FLASH(image)", flash_total, flash_used_images, flash_free_images);
    }

    size_t psram_chip_total = esp_psram_get_size();
    size_t psram_heap_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_heap_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_heap_used = (psram_heap_total >= psram_heap_free) ? (psram_heap_total - psram_heap_free) : 0;

    if (psram_chip_total > 0) {
        log_capacity_report_line("PSRAM(chip)",
                                 (uint64_t)psram_chip_total,
                                 (uint64_t)psram_heap_used,
                                 (uint64_t)psram_chip_total > psram_heap_used ? (uint64_t)psram_chip_total - psram_heap_used : 0);
    } else {
        ESP_LOGW(TAG, "MEM: PSRAM chip size is 0 (PSRAM init failed or unavailable)");
    }

    if (psram_heap_total > 0) {
        log_capacity_report_line("PSRAM(heap)", (uint64_t)psram_heap_total, (uint64_t)psram_heap_used, (uint64_t)psram_heap_free);
    } else {
        ESP_LOGW(TAG, "MEM: PSRAM heap not available");
    }
}

static void album_task(void *arg)
{
    (void)arg;
    const TickType_t wait_tick = pdMS_TO_TICKS(3000);
    char new_art_url[YTMD_ART_URL_MAX_LEN] = {0};
    char new_title[PLAYER_TITLE_MAX_LEN] = {0};
    char new_artist[PLAYER_ARTIST_MAX_LEN] = {0};
    ytmd_client_playback_state_t playback_state = {0};
    uint32_t last_aux_enrich_tick = 0;
    uint32_t last_queue_refresh_tick = 0;

    while (1) {
        if (!ncm_net_wait_ready(wait_tick)) {
            ESP_LOGI(TAG, "Waiting for USB attach/netif...");
            continue;
        }

        if (!ncm_net_is_ready()) {
            ESP_LOGW(TAG, "USB network not ready");
            continue;
        }

        esp_err_t err = ytmd_client_fetch_album_art(ui_display_get_album_buffer(),
                                                     UI_ALBUM_ART_W,
                                                     UI_ALBUM_ART_H,
                                                     s_last_art_url,
                                                     new_art_url,
                                                     sizeof(new_art_url),
                                                     new_title,
                                                     sizeof(new_title),
                                                     new_artist,
                                                     sizeof(new_artist),
                                                     &playback_state,
                                                     ncm_net_log_diagnostics);

        bool text_changed = false;
        bool playback_changed = false;



        if (err == ESP_OK) {
            bool waiting_for_new_track = false;
            if (s_pending_track_switch) {
                const bool has_track_text = (new_title[0] != '\0') || (new_artist[0] != '\0');
                const bool same_track = is_same_track_signature(new_title,
                                                                new_artist,
                                                                s_pending_from_title,
                                                                s_pending_from_artist);
                waiting_for_new_track = (!has_track_text) || same_track;
                if (waiting_for_new_track) {
                    uint32_t now_tick_switch = (uint32_t)xTaskGetTickCount();
                    if ((int32_t)(s_pending_track_switch_deadline_tick - now_tick_switch) <= 0) {
                        ESP_LOGW(TAG, "Track switch song-confirm timeout: dir=%d", s_pending_track_switch_dir);
                        maybe_resume_playback_after_switch("song_confirm_timeout");
                        clear_pending_track_switch();
                    }
                }
            }

            if (!waiting_for_new_track) {
                char display_title[PLAYER_TITLE_MAX_LEN] = {0};
                char display_artist[PLAYER_ARTIST_MAX_LEN] = {0};
                snprintf(display_title, sizeof(display_title), "%s", new_title);
                snprintf(display_artist, sizeof(display_artist), "%s", new_artist);
                prefer_queue_json_title_artist(display_title, sizeof(display_title), display_artist, sizeof(display_artist));

                text_changed |= update_text_field(s_player_state.title, sizeof(s_player_state.title), display_title);
                text_changed |= update_text_field(s_player_state.artist, sizeof(s_player_state.artist), display_artist);
                playback_changed = apply_playback_state_to_player(&playback_state);
                s_prev_transition_guard_until_tick = 0;
                if (s_pending_track_switch) {
                    maybe_resume_playback_after_switch((s_pending_track_switch_dir < 0) ? "prev_ready" : "next_ready");
                    clear_pending_track_switch();
                }
                ui_display_present_album_art();
                player_ui_update_album_art();
                snprintf(s_last_art_url, sizeof(s_last_art_url), "%s", new_art_url);
                player_ui_update(&s_player_state);
                ESP_LOGI(TAG, "Album art updated: %s", s_last_art_url);
                ESP_LOGI(TAG, "Now playing: %s - %s",
                         s_player_state.title[0] ? s_player_state.title : "-",
                         s_player_state.artist[0] ? s_player_state.artist : "-");
            }
        } else {
            bool suppress_transient_update = false;
            if (s_pending_track_switch) {
                uint32_t now_tick_switch = (uint32_t)xTaskGetTickCount();
                if ((int32_t)(s_pending_track_switch_deadline_tick - now_tick_switch) > 0) {
                    suppress_transient_update = true;
                } else {
                    ESP_LOGW(TAG, "Track switch wait timeout: dir=%d", s_pending_track_switch_dir);
                    maybe_resume_playback_after_switch("timeout");
                    clear_pending_track_switch();
                }
            }
            if (!s_pending_track_switch) {
                char display_title[PLAYER_TITLE_MAX_LEN] = {0};
                char display_artist[PLAYER_ARTIST_MAX_LEN] = {0};
                snprintf(display_title, sizeof(display_title), "%s", new_title);
                snprintf(display_artist, sizeof(display_artist), "%s", new_artist);
                prefer_queue_json_title_artist(display_title, sizeof(display_title), display_artist, sizeof(display_artist));

                text_changed |= update_text_field(s_player_state.title, sizeof(s_player_state.title), display_title);
                text_changed |= update_text_field(s_player_state.artist, sizeof(s_player_state.artist), display_artist);
                playback_changed = apply_playback_state_to_player(&playback_state);
            }
            if (err == ESP_ERR_NOT_FOUND && s_prev_transition_guard_until_tick != 0) {
                uint32_t now_tick_guard = (uint32_t)xTaskGetTickCount();
                if ((int32_t)(s_prev_transition_guard_until_tick - now_tick_guard) > 0) {
                    suppress_transient_update = true;
                } else {
                    s_prev_transition_guard_until_tick = 0;
                }
            }
            if (!suppress_transient_update && (text_changed || playback_changed)) {
                player_ui_update(&s_player_state);
                // ESP_LOGI(TAG, "Playback state updated: %s - %s",
                //          s_player_state.title[0] ? s_player_state.title : "-",
                //          s_player_state.artist[0] ? s_player_state.artist : "-");
            }
            if (err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Album art poll failed: %s", esp_err_to_name(err));
            }
        }

        uint32_t now_tick = (uint32_t)xTaskGetTickCount();
        if ((now_tick - last_queue_refresh_tick) >= pdMS_TO_TICKS(YTMD_QUEUE_REFRESH_INTERVAL_MS)) {
            esp_err_t qerr = ytmd_client_refresh_queue_cache(ncm_net_log_diagnostics);
            if (qerr != ESP_OK && qerr != ESP_ERR_NOT_FOUND && qerr != ESP_ERR_INVALID_SIZE) {
                ESP_LOGD(TAG, "Queue cache refresh skipped: %s", esp_err_to_name(qerr));
            }
            last_queue_refresh_tick = now_tick;
        }
        if (!s_pending_track_switch &&
            (now_tick - last_aux_enrich_tick) >= pdMS_TO_TICKS(YTMD_AUX_STATE_ENRICH_INTERVAL_MS)) {
            ytmd_client_playback_state_t aux_state = playback_state;
            if (ytmd_client_enrich_playback_state(&aux_state, ncm_net_log_diagnostics) == ESP_OK) {
                if (apply_playback_state_to_player(&aux_state)) {
                    player_ui_update(&s_player_state);
                }
            }
            last_aux_enrich_tick = now_tick;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(YTMD_POLL_INTERVAL_MS));
    }
}
void app_main(void)
{
#if !SOC_USB_OTG_SUPPORTED
    ESP_LOGE(TAG, "This target does not support USB OTG device mode.");
    return;
#else
    ESP_LOGI(TAG, "USB NCM + YTMD album art viewer start");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(ncm_net_init());
    ESP_ERROR_CHECK(ui_display_init());
    player_ui_init();
    const player_ui_control_ops_t control_ops = {
        .prev = ui_cmd_prev,
        .next = ui_cmd_next,
    };
    player_ui_set_control_ops(&control_ops, NULL);
    log_memory_capacity_report();

    BaseType_t ok = xTaskCreate(
        album_task,
        "album_task",
        12288,
        NULL,
        5,
        &s_album_task_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create album_task");
    }
#endif
}




