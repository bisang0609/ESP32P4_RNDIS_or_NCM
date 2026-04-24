#include "images.h"

const ext_img_desc_t images[18] = {
    { "skip_previous", &img_skip_previous },
    { "skip_next_", &img_skip_next_ },
    { "playlist", &img_playlist },
    { "play", &img_play },
    { "pause", &img_pause },
    { "List_enable", &img_list_enable },
    { "Repeat_ALL_enable", &img_repeat_all_enable },
    { "Repeat_disable", &img_repeat_disable },
    { "Repeat_One_enable", &img_repeat_one_enable },
    { "Shuffle_disable", &img_shuffle_disable },
    { "Shuffle_enable", &img_shuffle_enable },
    { "Thumb_Down_disable", &img_thumb_down_disable },
    { "Thumb_Down_enable", &img_thumb_down_enable },
    { "Thumb_Up_disable", &img_thumb_up_disable },
    { "Thumb_Up_enable", &img_thumb_up_enable },
    { "skip_next_push", &img_skip_next_push },
    { "skip_previous_push", &img_skip_previous_push },
    { "arrow_left", &img_arrow_left },
};