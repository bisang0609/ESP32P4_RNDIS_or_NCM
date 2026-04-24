#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_PLAYLIST = 2,
    _SCREEN_ID_LAST = 2
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *playlist;
    lv_obj_t *albumart_pan;
    lv_obj_t *album_art;
    lv_obj_t *song_title;
    lv_obj_t *song_artist;
    lv_obj_t *play;
    lv_obj_t *skip_next;
    lv_obj_t *skip_prev;
    lv_obj_t *song_random;
    lv_obj_t *song_repeat;
    lv_obj_t *song_senti;
    lv_obj_t *song_like;
    lv_obj_t *next;
    lv_obj_t *next_song;
    lv_obj_t *seekbar;
    lv_obj_t *time_now;
    lv_obj_t *total_time;
    lv_obj_t *golist;
    lv_obj_t *playlist_area;
    lv_obj_t *return_main;
    lv_obj_t *nowplay;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void create_screen_playlist();
void tick_screen_playlist();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

//
// Helper functions
//

lv_anim_t *get_anim();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/