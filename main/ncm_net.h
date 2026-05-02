#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t ncm_net_init(void);
bool ncm_net_wait_ready(TickType_t timeout_ticks);
bool ncm_net_is_ready(void);
void ncm_net_log_diagnostics(const char *url);
