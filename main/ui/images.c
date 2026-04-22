#include "images.h"

const ext_img_desc_t images[14] = {
    { "repeat_disable", &img_repeat_disable },
    { "repeat_one", &img_repeat_one },
    { "repeat_all", &img_repeat_all },
    { "like_enable", &img_like_enable },
    { "like_disenable", &img_like_disenable },
    { "skip_previous", &img_skip_previous },
    { "skip_next_", &img_skip_next_ },
    { "shu_enable", &img_shu_enable },
    { "shu_disenable", &img_shu_disenable },
    { "Sentiment Neutral_enable", &img_sentiment_neutral_enable },
    { "Sentiment Neutral_disable", &img_sentiment_neutral_disable },
    { "playlist", &img_playlist },
    { "play", &img_play },
    { "pause", &img_pause },
};