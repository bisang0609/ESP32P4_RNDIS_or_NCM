#include <string.h>

#include "screens.h"
#include "images.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"

#include <string.h>

objects_t objects;

// Global state variables

static lv_anim_t anim;
static bool anim_initialized;

//
// Helper functions
//

lv_anim_t *get_anim() {
    if (!anim_initialized) {
        lv_anim_init(&anim);
        lv_anim_set_delay(&anim, 1000);
        lv_anim_set_repeat_count(&anim, -1);
        anim_initialized = true;
    }
    return &anim;
}

//
// Event handlers
//

lv_obj_t *tick_value_change_obj;

//
// Screens
//

void create_screen_main() {
    lv_obj_t *obj = lv_obj_create(0);
    objects.main = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 800, 480);
    {
        lv_obj_t *parent_obj = obj;
        {
            // albumart_pan
            lv_obj_t *obj = lv_obj_create(parent_obj);
            objects.albumart_pan = obj;
            lv_obj_set_pos(obj, 20, 40);
            lv_obj_set_size(obj, 410, 410);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x790f0f), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x790f0f), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(obj, lv_color_hex(0x790f0f), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // album_art
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.album_art = obj;
            lv_obj_set_pos(obj, 25, 45);
            lv_obj_set_size(obj, 400, 400);
        }
        {
            // song_title
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.song_title = obj;
            lv_obj_set_pos(obj, 443, 86);
            lv_obj_set_size(obj, 320, 60);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_anim(obj, get_anim(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_34, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_anim_duration(obj, 30000, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        }
        {
            // song_artist
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.song_artist = obj;
            lv_obj_set_pos(obj, 442, 61);
            lv_obj_set_size(obj, 320, 40);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_anim(obj, get_anim(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x727272), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_anim_duration(obj, 30000, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "AAAAAAAAAAAAAAAAAAAAAAA");
        }
        {
            // play
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.play = obj;
            lv_obj_set_pos(obj, 557, 213);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_play);
        }
        {
            // skip_next
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.skip_next = obj;
            lv_obj_set_pos(obj, 663, 238);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_skip_next_);
            lv_image_set_scale(obj, 200);
        }
        {
            // skip_prev
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.skip_prev = obj;
            lv_obj_set_pos(obj, 501, 238);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_skip_previous);
            lv_image_set_scale(obj, 200);
        }
        {
            // song_random
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.song_random = obj;
            lv_obj_set_pos(obj, 424, 223);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_shuffle_disable);
            lv_image_set_scale(obj, 100);
        }
        {
            // song_repeat
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.song_repeat = obj;
            lv_obj_set_pos(obj, 709, 223);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_repeat_disable);
            lv_image_set_scale(obj, 100);
        }
        {
            // song_senti
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.song_senti = obj;
            lv_obj_set_pos(obj, 455, 303);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_thumb_down_disable);
            lv_image_set_scale(obj, 100);
        }
        {
            // song_like
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.song_like = obj;
            lv_obj_set_pos(obj, 683, 303);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_thumb_up_disable);
            lv_image_set_scale(obj, 100);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            lv_obj_set_pos(obj, 22, 455);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "YouTube Music Desktop Controler");
        }
        {
            // next
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.next = obj;
            lv_obj_set_pos(obj, 440, 420);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, 40);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x727272), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "Next");
        }
        {
            // next_song
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.next_song = obj;
            lv_obj_set_pos(obj, 515, 420);
            lv_obj_set_size(obj, 248, 40);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_anim(obj, get_anim(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x727272), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_anim_duration(obj, 30000, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "SSSSSSSSSSSSSSSSSSSSSS");
        }
        {
            // seekbar
            lv_obj_t *obj = lv_bar_create(parent_obj);
            objects.seekbar = obj;
            lv_obj_set_pos(obj, 446, 157);
            lv_obj_set_size(obj, 317, 10);
            lv_bar_set_value(obj, 25, LV_ANIM_OFF);
        }
        {
            // time_now
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.time_now = obj;
            lv_obj_set_pos(obj, 446, 175);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x727272), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "99:99");
        }
        {
            // total_time
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.total_time = obj;
            lv_obj_set_pos(obj, 723, 175);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x727272), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "99:99");
        }
        {
            // golist
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.golist = obj;
            lv_obj_set_pos(obj, 709, -8);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_list_enable);
            lv_image_set_scale(obj, 125);
        }
    }
    
    tick_screen_main();
}

void tick_screen_main() {
}

void create_screen_playlist() {
    lv_obj_t *obj = lv_obj_create(0);
    objects.playlist = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 800, 480);
    {
        lv_obj_t *parent_obj = obj;
        {
            // playlist_area
            lv_obj_t *obj = lv_obj_create(parent_obj);
            objects.playlist_area = obj;
            lv_obj_set_pos(obj, 54, 76);
            lv_obj_set_size(obj, 692, 384);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
            lv_obj_set_scroll_dir(obj, LV_DIR_VER);
            lv_obj_set_style_layout(obj, LV_LAYOUT_FLEX, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_layout(obj, LV_LAYOUT_FLEX, LV_PART_MAIN | LV_STATE_SCROLLED);
            lv_obj_set_style_flex_flow(obj, LV_FLEX_FLOW_COLUMN, LV_PART_MAIN | LV_STATE_SCROLLED);
        }
        {
            // return_main
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.return_main = obj;
            lv_obj_set_pos(obj, 8, -25);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_arrow_left);
            lv_image_set_scale(obj, 125);
        }
        {
            // nowplay
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.nowplay = obj;
            lv_obj_set_pos(obj, 104, 15);
            lv_obj_set_size(obj, 527, 40);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_pad_top(obj, 1, LV_PART_MAIN | LV_STATE_SCROLLED);
            lv_label_set_text_static(obj, "Title-Songs");
        }
        {
            // next_page
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.next_page = obj;
            lv_obj_set_pos(obj, 721, 18);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_next_page);
        }
        {
            // back_page
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.back_page = obj;
            lv_obj_set_pos(obj, 638, 18);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_back_page);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            lv_obj_set_pos(obj, 672, 23);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_SCROLLED);
            lv_label_set_text_static(obj, "PAGE");
        }
    }
    
    tick_screen_playlist();
}

void tick_screen_playlist() {
}

typedef void (*tick_screen_func_t)();
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
    tick_screen_playlist,
};
void tick_screen(int screen_index) {
    if (screen_index >= 0 && screen_index < 2) {
        tick_screen_funcs[screen_index]();
    }
}
void tick_screen_by_id(enum ScreensEnum screenId) {
    tick_screen(screenId - 1);
}

//
// Fonts
//

ext_font_desc_t fonts[] = {
#if LV_FONT_MONTSERRAT_8
    { "MONTSERRAT_8", &lv_font_montserrat_8 },
#endif
#if LV_FONT_MONTSERRAT_10
    { "MONTSERRAT_10", &lv_font_montserrat_10 },
#endif
#if LV_FONT_MONTSERRAT_12
    { "MONTSERRAT_12", &lv_font_montserrat_12 },
#endif
#if LV_FONT_MONTSERRAT_14
    { "MONTSERRAT_14", &lv_font_montserrat_14 },
#endif
#if LV_FONT_MONTSERRAT_16
    { "MONTSERRAT_16", &lv_font_montserrat_16 },
#endif
#if LV_FONT_MONTSERRAT_18
    { "MONTSERRAT_18", &lv_font_montserrat_18 },
#endif
#if LV_FONT_MONTSERRAT_20
    { "MONTSERRAT_20", &lv_font_montserrat_20 },
#endif
#if LV_FONT_MONTSERRAT_22
    { "MONTSERRAT_22", &lv_font_montserrat_22 },
#endif
#if LV_FONT_MONTSERRAT_24
    { "MONTSERRAT_24", &lv_font_montserrat_24 },
#endif
#if LV_FONT_MONTSERRAT_26
    { "MONTSERRAT_26", &lv_font_montserrat_26 },
#endif
#if LV_FONT_MONTSERRAT_28
    { "MONTSERRAT_28", &lv_font_montserrat_28 },
#endif
#if LV_FONT_MONTSERRAT_30
    { "MONTSERRAT_30", &lv_font_montserrat_30 },
#endif
#if LV_FONT_MONTSERRAT_32
    { "MONTSERRAT_32", &lv_font_montserrat_32 },
#endif
#if LV_FONT_MONTSERRAT_34
    { "MONTSERRAT_34", &lv_font_montserrat_34 },
#endif
#if LV_FONT_MONTSERRAT_36
    { "MONTSERRAT_36", &lv_font_montserrat_36 },
#endif
#if LV_FONT_MONTSERRAT_38
    { "MONTSERRAT_38", &lv_font_montserrat_38 },
#endif
#if LV_FONT_MONTSERRAT_40
    { "MONTSERRAT_40", &lv_font_montserrat_40 },
#endif
#if LV_FONT_MONTSERRAT_42
    { "MONTSERRAT_42", &lv_font_montserrat_42 },
#endif
#if LV_FONT_MONTSERRAT_44
    { "MONTSERRAT_44", &lv_font_montserrat_44 },
#endif
#if LV_FONT_MONTSERRAT_46
    { "MONTSERRAT_46", &lv_font_montserrat_46 },
#endif
#if LV_FONT_MONTSERRAT_48
    { "MONTSERRAT_48", &lv_font_montserrat_48 },
#endif
};

//
// Color themes
//

uint32_t active_theme_index = 0;

//
//
//

void create_screens() {

// Set default LVGL theme
    lv_display_t *dispp = lv_display_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, LV_FONT_DEFAULT);
    lv_display_set_theme(dispp, theme);
    
    // Initialize screens
    // Create screens
    create_screen_main();
    create_screen_playlist();
}