#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_skip_previous;
extern const lv_img_dsc_t img_skip_next_;
extern const lv_img_dsc_t img_playlist;
extern const lv_img_dsc_t img_play;
extern const lv_img_dsc_t img_pause;
extern const lv_img_dsc_t img_list_enable;
extern const lv_img_dsc_t img_repeat_all_enable;
extern const lv_img_dsc_t img_repeat_disable;
extern const lv_img_dsc_t img_repeat_one_enable;
extern const lv_img_dsc_t img_shuffle_disable;
extern const lv_img_dsc_t img_shuffle_enable;
extern const lv_img_dsc_t img_thumb_down_disable;
extern const lv_img_dsc_t img_thumb_down_enable;
extern const lv_img_dsc_t img_thumb_up_disable;
extern const lv_img_dsc_t img_thumb_up_enable;
extern const lv_img_dsc_t img_skip_next_push;
extern const lv_img_dsc_t img_skip_previous_push;
extern const lv_img_dsc_t img_arrow_left;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[18];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/