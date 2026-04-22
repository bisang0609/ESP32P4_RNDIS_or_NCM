#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_repeat_disable;
extern const lv_img_dsc_t img_repeat_one;
extern const lv_img_dsc_t img_repeat_all;
extern const lv_img_dsc_t img_like_enable;
extern const lv_img_dsc_t img_like_disenable;
extern const lv_img_dsc_t img_skip_previous;
extern const lv_img_dsc_t img_skip_next_;
extern const lv_img_dsc_t img_shu_enable;
extern const lv_img_dsc_t img_shu_disenable;
extern const lv_img_dsc_t img_sentiment_neutral_enable;
extern const lv_img_dsc_t img_sentiment_neutral_disable;
extern const lv_img_dsc_t img_playlist;
extern const lv_img_dsc_t img_play;
extern const lv_img_dsc_t img_pause;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[14];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/