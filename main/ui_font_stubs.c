#include "lvgl.h"

/*
 * Build-time fallback symbols for generated UI code.
 * Real fonts should be loaded at runtime (SPIFFS/binfont) in ui_display.c.
 */

const lv_font_t ui_font_font_27 __attribute__((weak)) = {0};
const lv_font_t ui_font_font_15 __attribute__((weak)) = {0};
