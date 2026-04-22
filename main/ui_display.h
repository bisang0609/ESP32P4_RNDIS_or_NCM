#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct _lv_font_t lv_font_t;

#define UI_ALBUM_ART_W 400
#define UI_ALBUM_ART_H 400

esp_err_t ui_display_init(void);
uint16_t *ui_display_get_album_buffer(void);
void ui_display_present_album_art(void);
const lv_font_t *ui_display_font_small(void);
const lv_font_t *ui_display_font_large(void);
