#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define YTMD_ART_URL_MAX_LEN 1024
#define YTMD_POLL_INTERVAL_MS 2000

typedef void (*ytmd_network_diag_cb_t)(const char *url);

esp_err_t ytmd_client_fetch_album_art(uint16_t *dst_rgb565,
                                      int dst_w,
                                      int dst_h,
                                      const char *last_art_url,
                                      char *out_art_url,
                                      size_t out_art_url_cap,
                                      ytmd_network_diag_cb_t net_diag_cb);
