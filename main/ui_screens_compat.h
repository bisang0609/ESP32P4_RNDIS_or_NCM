#pragma once

#include "lvgl.h"
#include "ui/fonts.h"

#if !defined(LV_FONT_MONTSERRAT_18) || (LV_FONT_MONTSERRAT_18 == 0)
/* LV_FONT_MONTSERRAT_18가 비활성인 빌드에서는 안전한 기본 폰트로 대체 */
#define lv_font_montserrat_18 lv_font_montserrat_14
#endif
