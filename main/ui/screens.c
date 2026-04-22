#include <string.h>

#include "screens.h"
#include "images.h"
#include "fonts.h"
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
            // albumart
            lv_obj_t *obj = lv_obj_create(parent_obj);
            objects.albumart = obj;
            lv_obj_set_pos(obj, 32, 40);
            lv_obj_set_size(obj, 400, 400);
        }
        {
            // song_title
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.song_title = obj;
            lv_obj_set_pos(obj, 450, 80);
            lv_obj_set_size(obj, 320, 56);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_anim(obj, get_anim(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "水平線 - Suiheisen입니다. 정말입니다.");
        }
        {
            // song_artist
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.song_artist = obj;
            lv_obj_set_pos(obj, 450, 121);
            lv_obj_set_size(obj, 320, 30);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_anim(obj, get_anim(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x727272), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "여기는 가수가 들어갑니다. 정말입니다................................................................");
        }
        {
            // play
            lv_obj_t *obj = lv_img_create(parent_obj);
            objects.play = obj;
            lv_obj_set_pos(obj, 567, 200);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_img_set_src(obj, &img_play);
        }
        {
            // skip_next
            lv_obj_t *obj = lv_img_create(parent_obj);
            objects.skip_next = obj;
            lv_obj_set_pos(obj, 710, 225);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_img_set_src(obj, &img_skip_next_);
        }
        {
            // skip_prev
            lv_obj_t *obj = lv_img_create(parent_obj);
            objects.skip_prev = obj;
            lv_obj_set_pos(obj, 474, 225);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_img_set_src(obj, &img_skip_previous);
        }
        {
            // song_random
            lv_obj_t *obj = lv_img_create(parent_obj);
            objects.song_random = obj;
            lv_obj_set_pos(obj, 474, 353);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_img_set_src(obj, &img_shu_disenable);
            lv_img_set_zoom(obj, 120);
        }
        {
            // song_repeat
            lv_obj_t *obj = lv_img_create(parent_obj);
            objects.song_repeat = obj;
            lv_obj_set_pos(obj, 553, 353);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_img_set_src(obj, &img_repeat_disable);
            lv_img_set_zoom(obj, 120);
        }
        {
            // song_senti
            lv_obj_t *obj = lv_img_create(parent_obj);
            objects.song_senti = obj;
            lv_obj_set_pos(obj, 710, 353);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_img_set_src(obj, &img_sentiment_neutral_disable);
            lv_img_set_zoom(obj, 150);
        }
        {
            // song_like
            lv_obj_t *obj = lv_img_create(parent_obj);
            objects.song_like = obj;
            lv_obj_set_pos(obj, 631, 353);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_img_set_src(obj, &img_like_disenable);
            lv_img_set_zoom(obj, 150);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            lv_obj_set_pos(obj, 32, 15);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_label_set_text_static(obj, "YouTube Music Desktop Controler");
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.obj0 = obj;
            lv_obj_set_pos(obj, 442, 411);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x727272), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "Next");
        }
        {
            // next_song
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.next_song = obj;
            lv_obj_set_pos(obj, 482, 411);
            lv_obj_set_size(obj, 288, LV_SIZE_CONTENT);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_anim(obj, get_anim(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x727272), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "다음가수 들어가는 자리입니다..............................................................");
        }
    }
    
    tick_screen_main();
}

void tick_screen_main() {
}

typedef void (*tick_screen_func_t)();
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
};
void tick_screen(int screen_index) {
    if (screen_index >= 0 && screen_index < 1) {
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
    { "font_27", &ui_font_font_27 },
    { "font_15", &ui_font_font_15 },
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
    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);
    
    // Initialize screens
    // Create screens
    create_screen_main();
}