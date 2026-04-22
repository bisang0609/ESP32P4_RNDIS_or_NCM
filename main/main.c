/*
 * USB-NCM YTMD album art viewer for ESP32-P4.
 *
 * High-level flow:
 * 1) Bring up USB-NCM network link to host
 * 2) Poll YTMD /api/v1/song
 * 3) Download/prepare album art
 * 4) Render album art on LVGL UI
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "soc/soc_caps.h"

#include "ncm_net.h"
#include "ui_display.h"
#include "ytmd_client.h"

static const char *TAG = "USB_NCM_YTMD_ART";

static char s_last_art_url[YTMD_ART_URL_MAX_LEN] = {0};

static void format_mb_decimal(uint64_t bytes, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    uint64_t milli_mb = (bytes * 1000ULL + 500000ULL) / 1000000ULL;
    snprintf(out, out_len, "%" PRIu64 ".%03" PRIu64, milli_mb / 1000ULL, milli_mb % 1000ULL);
}

static void format_pct_x100(uint64_t used, uint64_t total, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (total == 0) {
        snprintf(out, out_len, "0.00");
        return;
    }
    uint64_t pct_x100 = (used * 10000ULL + total / 2ULL) / total;
    snprintf(out, out_len, "%" PRIu64 ".%02" PRIu64, pct_x100 / 100ULL, pct_x100 % 100ULL);
}

static void log_capacity_report_line(const char *name, uint64_t total, uint64_t used, uint64_t free_bytes)
{
    char total_mb[24] = {0};
    char used_mb[24] = {0};
    char free_mb[24] = {0};
    char used_pct[16] = {0};

    format_mb_decimal(total, total_mb, sizeof(total_mb));
    format_mb_decimal(used, used_mb, sizeof(used_mb));
    format_mb_decimal(free_bytes, free_mb, sizeof(free_mb));
    format_pct_x100(used, total, used_pct, sizeof(used_pct));

    ESP_LOGI(TAG, "%s: total=%" PRIu64 "B(%sMB) used=%" PRIu64 "B(%sMB,%s%%) free=%" PRIu64 "B(%sMB)",
             name, total, total_mb, used, used_mb, used_pct, free_bytes, free_mb);
}

static uint64_t get_flash_partition_footprint_end(void)
{
    uint64_t max_end = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p) {
            continue;
        }
        uint64_t end = (uint64_t)p->address + (uint64_t)p->size;
        if (end > max_end) {
            max_end = end;
        }
    }
    return max_end;
}

static uint32_t get_running_app_image_len(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGW(TAG, "MEM: running partition not found");
        return 0;
    }

    esp_partition_pos_t pos = {
        .offset = running->address,
        .size = running->size,
    };
    esp_image_metadata_t md = {0};
    esp_err_t err = esp_image_get_metadata(&pos, &md);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MEM: esp_image_get_metadata failed for running app: %s", esp_err_to_name(err));
        return 0;
    }
    return md.image_len;
}

static void log_memory_capacity_report(void)
{
    uint32_t flash_total_u32 = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_total_u32);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MEM: esp_flash_get_size failed: %s", esp_err_to_name(err));
    } else {
        const uint64_t flash_total = flash_total_u32;
        const uint64_t footprint_end = get_flash_partition_footprint_end();
        const uint64_t mandatory_end = (uint64_t)ESP_PARTITION_TABLE_OFFSET + (uint64_t)ESP_PARTITION_TABLE_SIZE;
        uint64_t flash_used_alloc = (footprint_end > mandatory_end) ? footprint_end : mandatory_end;
        if (flash_used_alloc > flash_total) {
            flash_used_alloc = flash_total;
        }
        uint64_t flash_free_alloc = flash_total - flash_used_alloc;

        uint32_t bootloader_len = 0;
        err = esp_image_verify_bootloader(&bootloader_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MEM: esp_image_verify_bootloader failed: %s", esp_err_to_name(err));
            bootloader_len = 0;
        }
        uint64_t app_image_len = get_running_app_image_len();
        uint64_t flash_used_images = (uint64_t)bootloader_len + (uint64_t)ESP_PARTITION_TABLE_SIZE + app_image_len;
        if (flash_used_images > flash_total) {
            flash_used_images = flash_total;
        }
        uint64_t flash_free_images = flash_total - flash_used_images;

        log_capacity_report_line("FLASH(alloc)", flash_total, flash_used_alloc, flash_free_alloc);
        log_capacity_report_line("FLASH(image)", flash_total, flash_used_images, flash_free_images);
    }

    size_t psram_chip_total = esp_psram_get_size();
    size_t psram_heap_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_heap_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_heap_used = (psram_heap_total >= psram_heap_free) ? (psram_heap_total - psram_heap_free) : 0;

    if (psram_chip_total > 0) {
        log_capacity_report_line("PSRAM(chip)",
                                 (uint64_t)psram_chip_total,
                                 (uint64_t)psram_heap_used,
                                 (uint64_t)psram_chip_total > psram_heap_used ? (uint64_t)psram_chip_total - psram_heap_used : 0);
    } else {
        ESP_LOGW(TAG, "MEM: PSRAM chip size is 0 (PSRAM init failed or unavailable)");
    }

    if (psram_heap_total > 0) {
        log_capacity_report_line("PSRAM(heap)", (uint64_t)psram_heap_total, (uint64_t)psram_heap_used, (uint64_t)psram_heap_free);
    } else {
        ESP_LOGW(TAG, "MEM: PSRAM heap not available");
    }
}

static void album_task(void *arg)
{
    (void)arg;
    const TickType_t wait_tick = pdMS_TO_TICKS(3000);
    char new_art_url[YTMD_ART_URL_MAX_LEN] = {0};

    while (1) {
        if (!ncm_net_wait_ready(wait_tick)) {
            ESP_LOGI(TAG, "Waiting for USB attach/netif...");
            continue;
        }

        if (!ncm_net_is_ready()) {
            ESP_LOGW(TAG, "USB network not ready");
            continue;
        }

        esp_err_t err = ytmd_client_fetch_album_art(ui_display_get_album_buffer(),
                                                     UI_ALBUM_ART_W,
                                                     UI_ALBUM_ART_H,
                                                     s_last_art_url,
                                                     new_art_url,
                                                     sizeof(new_art_url),
                                                     ncm_net_log_diagnostics);
        if (err == ESP_OK) {
            ui_display_present_album_art();
            snprintf(s_last_art_url, sizeof(s_last_art_url), "%s", new_art_url);
            ESP_LOGI(TAG, "Album art updated: %s", s_last_art_url);
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Album art poll failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(YTMD_POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
#if !SOC_USB_OTG_SUPPORTED
    ESP_LOGE(TAG, "This target does not support USB OTG device mode.");
    return;
#else
    ESP_LOGI(TAG, "USB NCM + YTMD album art viewer start");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(ncm_net_init());
    ESP_ERROR_CHECK(ui_display_init());
    log_memory_capacity_report();

    BaseType_t ok = xTaskCreate(
        album_task,
        "album_task",
        12288,
        NULL,
        5,
        NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create album_task");
    }
#endif
}
