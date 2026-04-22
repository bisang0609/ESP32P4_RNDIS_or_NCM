/*
 * USB-NCM YTMD album art viewer for ESP32-P4.
 *
 * Flow:
 * 1) Enumerate as USB-NCM device to host (Windows static IP setup)
 * 2) Poll YTMD /api/v1/song on host
 * 3) Extract album-art URL (no auth)
 * 4) Download JPEG art, decode with P4 HW JPEG decoder
 * 5) Scale/crop to panel and show only album art on LVGL screen
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "soc/soc_caps.h"

#include "driver/jpeg_decode.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "lvgl.h"

#include "lwip/esp_netif_net_stack.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

static const char *TAG = "USB_NCM_YTMD_ART";

#define EV_USB_ATTACHED      BIT0
#define EV_USB_NETIF_READY   BIT1

/* USB static IP plan
 * Host: 192.168.137.1/24
 * P4  : 192.168.137.2/24
 */
#define USB_LOCAL_IP_STR     "192.168.137.2"
#define USB_GW_IP_STR        "192.168.137.1"
#define USB_NETMASK_STR      "255.255.255.0"
#define USB_DNS_MAIN_STR     "8.8.8.8"
#define USB_DNS_BACKUP_STR   "1.1.1.1"

#define YTMD_URL_API         "http://192.168.137.1:26538/api/v1/song"

#define HTTP_TIMEOUT_MS      7000
#define MAX_JSON_BYTES       (64 * 1024)
#define MAX_IMAGE_BYTES      (3 * 1024 * 1024)
#define POLL_INTERVAL_MS     2000

#define ART_URL_MAX_LEN      1024

#define ALBUM_ART_W          400
#define ALBUM_ART_H          400
#define ART_SRC_REQ_W        400
#define ART_SRC_REQ_H        400
#define YTIMG_FALLBACK_FILE  "sddefault.jpg"

static EventGroupHandle_t s_ev = NULL;
static esp_netif_t *s_usb_netif = NULL;

static lv_obj_t *s_album_img = NULL;
static lv_img_dsc_t s_album_dsc;
static uint16_t *s_album_frame = NULL;
static char s_last_art_url[ART_URL_MAX_LEN] = {0};

static jpeg_decoder_handle_t s_jpeg_decoder = NULL;
static TickType_t s_last_net_diag_tick = 0;

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
        log_capacity_report_line("PSRAM(chip)", (uint64_t)psram_chip_total, (uint64_t)psram_heap_used,
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

static void copy_cstr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

static void append_cstr(char *dst, size_t cap, const char *suffix)
{
    if (!dst || !suffix || cap == 0) {
        return;
    }
    size_t len = strlen(dst);
    if (len >= cap - 1) {
        return;
    }
    snprintf(dst + len, cap - len, "%s", suffix);
}

static char *dup_cstr(const char *src)
{
    if (!src) {
        return NULL;
    }
    size_t n = strlen(src);
    char *dst = (char *)malloc(n + 1);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, n + 1);
    return dst;
}

static void tinyusb_event_handler(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (!s_ev || !event) {
        return;
    }

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "TinyUSB event: ATTACHED");
        xEventGroupSetBits(s_ev, EV_USB_ATTACHED);
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGW(TAG, "TinyUSB event: DETACHED");
        xEventGroupClearBits(s_ev, EV_USB_ATTACHED);
        break;
    default:
        ESP_LOGI(TAG, "TinyUSB event: id=%d", event->id);
        break;
    }
}

static void usb_l2_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t usb_netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (len > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tinyusb_net_send_sync failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_usb_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    void *buf_copy = malloc(len);
    if (!buf_copy) {
        ESP_LOGE(TAG, "RX malloc failed");
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf_copy, buffer, len);

    esp_err_t err = esp_netif_receive(s_usb_netif, buf_copy, len, NULL);
    if (err != ESP_OK) {
        free(buf_copy);
        ESP_LOGW(TAG, "esp_netif_receive failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t parse_static_ip(esp_netif_ip_info_t *ip_info)
{
    ESP_RETURN_ON_FALSE(ip_info, ESP_ERR_INVALID_ARG, TAG, "ip_info is NULL");

    memset(ip_info, 0, sizeof(*ip_info));
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_LOCAL_IP_STR, &ip_info->ip), TAG, "Invalid local IP");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_GW_IP_STR, &ip_info->gw), TAG, "Invalid gateway IP");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_NETMASK_STR, &ip_info->netmask), TAG, "Invalid netmask");
    return ESP_OK;
}

static esp_err_t configure_usb_netif_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dns_info_t dns_main = {0};
    esp_netif_dns_info_t dns_backup = {0};
    ESP_RETURN_ON_ERROR(parse_static_ip(&ip_info), TAG, "parse_static_ip failed");

    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_stop: %s", esp_err_to_name(err));
    }

    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip_info), TAG, "esp_netif_set_ip_info failed");
    dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_DNS_MAIN_STR, &dns_main.ip.u_addr.ip4), TAG, "Invalid main DNS");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_DNS_BACKUP_STR, &dns_backup.ip.u_addr.ip4), TAG, "Invalid backup DNS");
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main), TAG, "esp_netif_set_dns_info main failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup), TAG, "esp_netif_set_dns_info backup failed");

    ESP_LOGI(TAG, "USB netif static IP set: ip=" IPSTR " gw=" IPSTR " mask=" IPSTR,
             IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));
    ESP_LOGI(TAG, "USB netif DNS set: main=" IPSTR " backup=" IPSTR,
             IP2STR(&dns_main.ip.u_addr.ip4), IP2STR(&dns_backup.ip.u_addr.ip4));
    return ESP_OK;
}

static esp_err_t init_usb_ncm_and_netif(void)
{
    ESP_LOGI(TAG, "Installing TinyUSB driver...");
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tinyusb_event_handler);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install failed");

    tinyusb_net_config_t net_cfg = {
        .mac_addr = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01},
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer = NULL,
        .user_context = NULL,
    };

    ESP_LOGI(TAG, "Initializing TinyUSB NCM class...");
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&net_cfg), TAG, "tinyusb_net_init failed");

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_FLAG_AUTOUP | ESP_NETIF_FLAG_GARP,
        .if_key = "usb_ncm0",
        .if_desc = "usb_ncm_ytmd",
        .route_prio = 10,
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,
        .transmit = usb_netif_transmit,
        .driver_free_rx_buffer = usb_l2_free,
    };

    struct esp_netif_netstack_config netstack_cfg = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &netstack_cfg,
    };

    s_usb_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_usb_netif, ESP_ERR_NO_MEM, TAG, "esp_netif_new failed");

    uint8_t lwip_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x02};
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_usb_netif, lwip_mac), TAG, "esp_netif_set_mac failed");

    ESP_RETURN_ON_ERROR(configure_usb_netif_ip(s_usb_netif), TAG, "configure_usb_netif_ip failed");

    esp_netif_action_start(s_usb_netif, 0, 0, 0);
    esp_err_t err = esp_netif_set_default_netif(s_usb_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_set_default_netif failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "USB netif selected as default route");
    }

    xEventGroupSetBits(s_ev, EV_USB_NETIF_READY);
    return ESP_OK;
}

static bool usb_network_ready(void)
{
    EventBits_t bits = xEventGroupGetBits(s_ev);
    if ((bits & (EV_USB_ATTACHED | EV_USB_NETIF_READY)) != (EV_USB_ATTACHED | EV_USB_NETIF_READY)) {
        return false;
    }
    if (!s_usb_netif || !esp_netif_is_netif_up(s_usb_netif)) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_usb_netif, &ip_info) != ESP_OK) {
        return false;
    }
    return (ip_info.ip.addr != 0);
}

static void replace_all(char *buf, size_t cap, const char *from, const char *to)
{
    if (!buf || !from || !to) {
        return;
    }

    const size_t from_len = strlen(from);
    const size_t to_len = strlen(to);
    if (from_len == 0) {
        return;
    }

    char *pos = strstr(buf, from);
    while (pos) {
        size_t buf_len = strlen(buf);
        size_t tail_len = strlen(pos + from_len);

        if (to_len > from_len) {
            size_t grow = to_len - from_len;
            if (buf_len + grow + 1 > cap) {
                break;
            }
        }

        memmove(pos + to_len, pos + from_len, tail_len + 1);
        memcpy(pos, to, to_len);
        pos = strstr(pos + to_len, from);
    }
}

static void normalize_art_url(char *url, size_t cap)
{
    if (!url || cap == 0) {
        return;
    }

    if (strstr(url, "googleusercontent.com") || strstr(url, "ytimg.com") || strstr(url, "ggpht.com")) {
        char *eq = strrchr(url, '=');
        if (eq && eq[1] && (eq[1] == 'w' || eq[1] == 's')) {
            char size_suffix[32] = {0};
            snprintf(size_suffix, sizeof(size_suffix), "=w%d-h%d", ART_SRC_REQ_W, ART_SRC_REQ_H);
            *eq = '\0';
            append_cstr(url, cap, size_suffix);
        }
    }

    if (strstr(url, "i.ytimg.com/vi/")) {
        replace_all(url, cap, "/maxresdefault.jpg", "/" YTIMG_FALLBACK_FILE);
        replace_all(url, cap, "/hq720.jpg", "/" YTIMG_FALLBACK_FILE);
        replace_all(url, cap, "/hq720_live.jpg", "/" YTIMG_FALLBACK_FILE);
        replace_all(url, cap, "/hqdefault.jpg", "/" YTIMG_FALLBACK_FILE);
        replace_all(url, cap, "/mqdefault.jpg", "/" YTIMG_FALLBACK_FILE);
    }
}

static bool is_jpeg_data(const uint8_t *buf, size_t len)
{
    if (!buf || len < 3) {
        return false;
    }
    return (buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF);
}

static bool make_ytimg_fallback_url(const char *video_id, char *out, size_t cap)
{
    if (!video_id || !out || cap < 40) {
        return false;
    }

    size_t n = strlen(video_id);
    if (n == 0 || n > 32) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)video_id[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
    }

    int w = snprintf(out, cap, "https://i.ytimg.com/vi/%s/%s", video_id, YTIMG_FALLBACK_FILE);
    return (w > 0 && (size_t)w < cap);
}

static bool is_valid_video_id_char(char c)
{
    unsigned char u = (unsigned char)c;
    return (isalnum(u) || u == '_' || u == '-');
}

static bool is_valid_youtube_video_id(const char *s, size_t n)
{
    if (!s || n != 11) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!is_valid_video_id_char(s[i])) {
            return false;
        }
    }
    return true;
}

static char *dup_video_id_if_valid(const char *candidate)
{
    if (!candidate) {
        return NULL;
    }
    size_t n = strlen(candidate);
    if (!is_valid_youtube_video_id(candidate, n)) {
        return NULL;
    }
    return dup_cstr(candidate);
}

static bool extract_video_id_from_watch_like_url(const char *url, char *out, size_t out_cap)
{
    if (!url || !out || out_cap < 12) {
        return false;
    }

    const char *p = strstr(url, "watch?v=");
    if (p) {
        p += strlen("watch?v=");
    } else if ((p = strstr(url, "youtu.be/")) != NULL) {
        p += strlen("youtu.be/");
    } else if ((p = strstr(url, "/shorts/")) != NULL) {
        p += strlen("/shorts/");
    } else {
        return false;
    }

    size_t n = 0;
    while (p[n] && p[n] != '&' && p[n] != '?' && p[n] != '/' && n < 11) {
        n++;
    }
    if (!is_valid_youtube_video_id(p, n)) {
        return false;
    }

    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool extract_host_from_url(const char *url, char *host, size_t host_cap)
{
    if (!url || !host || host_cap < 2) {
        return false;
    }

    host[0] = '\0';
    const char *p = strstr(url, "://");
    p = p ? (p + 3) : url;

    const char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') {
        end++;
    }

    size_t n = (size_t)(end - p);
    if (n == 0 || n >= host_cap) {
        return false;
    }

    memcpy(host, p, n);
    host[n] = '\0';
    return true;
}

static void log_network_diagnostics(const char *url)
{
    TickType_t now = xTaskGetTickCount();
    if (s_last_net_diag_tick != 0 && (now - s_last_net_diag_tick) < pdMS_TO_TICKS(15000)) {
        return;
    }
    s_last_net_diag_tick = now;

    if (!s_usb_netif) {
        ESP_LOGW(TAG, "NET DIAG: usb netif is NULL");
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_usb_netif, &ip_info) == ESP_OK) {
        ESP_LOGW(TAG, "NET DIAG: ip=" IPSTR " gw=" IPSTR " mask=" IPSTR,
                 IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));
    } else {
        ESP_LOGW(TAG, "NET DIAG: failed to read IP info");
    }

    esp_netif_dns_info_t dns_main = {0};
    esp_netif_dns_info_t dns_backup = {0};
    esp_netif_dns_info_t dns_fallback = {0};
    esp_netif_get_dns_info(s_usb_netif, ESP_NETIF_DNS_MAIN, &dns_main);
    esp_netif_get_dns_info(s_usb_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
    esp_netif_get_dns_info(s_usb_netif, ESP_NETIF_DNS_FALLBACK, &dns_fallback);
    ESP_LOGW(TAG, "NET DIAG: dns main=" IPSTR " backup=" IPSTR " fallback=" IPSTR,
             IP2STR(&dns_main.ip.u_addr.ip4),
             IP2STR(&dns_backup.ip.u_addr.ip4),
             IP2STR(&dns_fallback.ip.u_addr.ip4));

    char host[128] = {0};
    if (!extract_host_from_url(url, host, sizeof(host))) {
        ESP_LOGW(TAG, "NET DIAG: unable to parse host from url=%s", url ? url : "(null)");
        return;
    }

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *ai = NULL;
    int r = getaddrinfo(host, NULL, &hints, &ai);
    ESP_LOGW(TAG, "NET DIAG: getaddrinfo(%s) => %d, ai=%p", host, r, ai);
    if (ai) {
        freeaddrinfo(ai);
    }
}

static char *extract_quoted_json_value_after_key(const char *json, const char *key_token)
{
    if (!json || !key_token) {
        return NULL;
    }

    const char *p = json;
    while ((p = strstr(p, key_token)) != NULL) {
        const char *colon = strchr(p + strlen(key_token), ':');
        if (!colon) {
            break;
        }
        const char *v = colon + 1;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') {
            v++;
        }
        if (*v != '"') {
            p = v;
            continue;
        }
        v++;

        char *out = (char *)malloc(ART_URL_MAX_LEN);
        if (!out) {
            return NULL;
        }

        size_t n = 0;
        bool esc = false;
        while (*v) {
            char c = *v++;
            if (esc) {
                if (n + 1 < ART_URL_MAX_LEN) {
                    out[n++] = c;
                }
                esc = false;
                continue;
            }
            if (c == '\\') {
                esc = true;
                continue;
            }
            if (c == '"') {
                break;
            }
            if (n + 1 < ART_URL_MAX_LEN) {
                out[n++] = c;
            }
        }
        out[n] = '\0';

        if (strncmp(out, "http://", 7) == 0 || strncmp(out, "https://", 8) == 0) {
            return out;
        }
        free(out);
        p = v;
    }
    return NULL;
}

static char *extract_first_url_token(const char *json)
{
    if (!json) {
        return NULL;
    }

    const char *p = strstr(json, "https://");
    const char *q = strstr(json, "http://");
    if (!p || (q && q < p)) {
        p = q;
    }
    if (!p) {
        return NULL;
    }

    char *out = (char *)malloc(ART_URL_MAX_LEN);
    if (!out) {
        return NULL;
    }

    size_t n = 0;
    while (*p && *p != '"' && *p != '\\' && *p != ' ' && n + 1 < ART_URL_MAX_LEN) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    if (n == 0) {
        free(out);
        return NULL;
    }
    return out;
}

static char *extract_art_url_from_song_json(const uint8_t *json_data, size_t json_len)
{
    if (!json_data || json_len == 0) {
        return NULL;
    }

    char *json = (char *)malloc(json_len + 1);
    if (!json) {
        return NULL;
    }
    memcpy(json, json_data, json_len);
    json[json_len] = '\0';

    char *url = extract_quoted_json_value_after_key(json, "\"cover\"");
    if (!url) {
        url = extract_quoted_json_value_after_key(json, "\"imageSrc\"");
    }
    if (!url) {
        url = extract_quoted_json_value_after_key(json, "\"url\"");
    }
    if (!url) {
        url = extract_first_url_token(json);
    }

    if (url) {
        normalize_art_url(url, ART_URL_MAX_LEN);
    }

    free(json);
    return url;
}

static char *extract_video_id_from_song_json(const uint8_t *json_data, size_t json_len)
{
    if (!json_data || json_len == 0) {
        return NULL;
    }

    char *json = (char *)malloc(json_len + 1);
    if (!json) {
        return NULL;
    }
    memcpy(json, json_data, json_len);
    json[json_len] = '\0';

    char *video_id = extract_quoted_json_value_after_key(json, "\"videoId\"");
    if (video_id) {
        char *validated = dup_video_id_if_valid(video_id);
        free(video_id);
        if (validated) {
            free(json);
            return validated;
        }
    }

    video_id = extract_quoted_json_value_after_key(json, "\"id\"");
    if (video_id) {
        char *validated = dup_video_id_if_valid(video_id);
        free(video_id);
        if (validated) {
            free(json);
            return validated;
        }
    }

    const char *p = json;
    while ((p = strstr(p, "http")) != NULL) {
        char token[ART_URL_MAX_LEN] = {0};
        size_t n = 0;
        while (p[n] && p[n] != '"' && p[n] != '\\' && p[n] != ' ' && n + 1 < sizeof(token)) {
            token[n] = p[n];
            n++;
        }
        token[n] = '\0';

        char vid[16] = {0};
        if (extract_video_id_from_watch_like_url(token, vid, sizeof(vid))) {
            free(json);
            return dup_cstr(vid);
        }
        p += (n > 0 ? n : 1);
    }

    free(json);
    return NULL;
}

static esp_err_t http_get_alloc(const char *url, size_t max_size, int timeout_ms, bool text_mode,
                                uint8_t **out_buf, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(url && out_buf && out_len, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "esp_http_client_init failed");
    if (!text_mode) {
        (void)esp_http_client_set_header(client, "Accept", "image/jpeg,image/*;q=0.9,*/*;q=0.8");
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed for %s: %s", url, esp_err_to_name(err));
        int tls_code = 0;
        int tls_flags = 0;
        if (esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags) == ESP_OK) {
            ESP_LOGW(TAG, "TLS detail: esp_tls_code=0x%x flags=0x%x", tls_code, tls_flags);
        }
        if (url && strncmp(url, "https://", 8) == 0) {
            log_network_diagnostics(url);
        }
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *content_type = NULL;
    (void)esp_http_client_get_header(client, "Content-Type", &content_type);
    if (!text_mode) {
        ESP_LOGI(TAG, "HTTP image headers: status=%d len=%lld ct=%s",
                 status, (long long)content_len, content_type ? content_type : "(null)");
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP status=%d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t reserve = 4096;
    if (content_len > 0 && (size_t)content_len < max_size) {
        reserve = (size_t)content_len + (text_mode ? 1u : 0u);
    }
    if (reserve == 0) {
        reserve = 1;
    }

    uint8_t *buf = (uint8_t *)malloc(reserve);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    int read_len = 0;
    uint8_t tmp[1024];

    while ((read_len = esp_http_client_read(client, (char *)tmp, sizeof(tmp))) > 0) {
        size_t need = used + (size_t)read_len + (text_mode ? 1u : 0u);
        if (need > max_size) {
            ESP_LOGW(TAG, "HTTP body too large: %u > %u", (unsigned)need, (unsigned)max_size);
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
        if (need > reserve) {
            size_t new_cap = reserve;
            while (new_cap < need) {
                new_cap *= 2;
            }
            uint8_t *new_buf = (uint8_t *)realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            buf = new_buf;
            reserve = new_cap;
        }
        memcpy(buf + used, tmp, (size_t)read_len);
        used += (size_t)read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len < 0) {
        free(buf);
        return ESP_FAIL;
    }

    if (!text_mode && used >= 4) {
        ESP_LOGI(TAG, "HTTP image magic: %02X %02X %02X %02X (size=%u)",
                 buf[0], buf[1], buf[2], buf[3], (unsigned)used);
    }

    if (text_mode) {
        buf[used] = '\0';
    }

    *out_buf = buf;
    *out_len = used;
    return ESP_OK;
}

static inline int align_up_int(int value, int alignment)
{
    return ((value + alignment - 1) / alignment) * alignment;
}

static void get_jpeg_mcu_size(jpeg_down_sampling_type_t sample_method, int *mcux, int *mcuy)
{
    switch (sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444:
    case JPEG_DOWN_SAMPLING_GRAY:
        *mcux = 8;
        *mcuy = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        *mcux = 16;
        *mcuy = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV420:
    default:
        *mcux = 16;
        *mcuy = 16;
        break;
    }
}

static esp_err_t ensure_jpeg_engine(void)
{
    if (s_jpeg_decoder) {
        return ESP_OK;
    }
    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms = 120,
    };
    return jpeg_new_decoder_engine(&cfg, &s_jpeg_decoder);
}

static bool decode_jpeg_rgb565(const uint8_t *jpg, size_t jpg_len,
                               uint16_t **out_pixels, int *out_w, int *out_h, int *out_stride)
{
    if (!jpg || !out_pixels || !out_w || !out_h || !out_stride || jpg_len < 4) {
        return false;
    }

    *out_pixels = NULL;
    *out_w = 0;
    *out_h = 0;
    *out_stride = 0;

    if (ensure_jpeg_engine() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init JPEG decoder engine");
        return false;
    }

    jpeg_decode_picture_info_t info = {0};
    if (jpeg_decoder_get_info(jpg, (uint32_t)jpg_len, &info) != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_get_info failed");
        return false;
    }

    if (info.width == 0 || info.height == 0 || info.width > 4096 || info.height > 4096) {
        ESP_LOGW(TAG, "Invalid JPEG size: %ux%u", (unsigned)info.width, (unsigned)info.height);
        return false;
    }

    const bool grayscale = (info.sample_method == JPEG_DOWN_SAMPLING_GRAY);
    int mcu_x = 16;
    int mcu_y = 16;
    get_jpeg_mcu_size(info.sample_method, &mcu_x, &mcu_y);

    const int dec_stride = align_up_int((int)info.width, mcu_x);
    const int out_alloc_h = align_up_int((int)info.height, mcu_y);

    size_t bpp = grayscale ? 1u : 2u;
    size_t raw_size = (size_t)dec_stride * (size_t)out_alloc_h * bpp;

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t alloc_sz = 0;
    uint8_t *raw_out = (uint8_t *)jpeg_alloc_decoder_mem(raw_size, &mem_cfg, &alloc_sz);
    if (!raw_out) {
        ESP_LOGE(TAG, "jpeg_alloc_decoder_mem failed (%u bytes)", (unsigned)raw_size);
        return false;
    }

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = grayscale ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_jpeg_decoder, &dec_cfg, jpg, (uint32_t)jpg_len,
                                         raw_out, (uint32_t)alloc_sz, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_process failed: %s", esp_err_to_name(err));
        free(raw_out);
        return false;
    }

    if (grayscale) {
        size_t rgb_size = (size_t)dec_stride * (size_t)out_alloc_h * 2u;
        uint16_t *rgb565 = (uint16_t *)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rgb565) {
            rgb565 = (uint16_t *)malloc(rgb_size);
        }
        if (!rgb565) {
            free(raw_out);
            return false;
        }
        for (int i = 0; i < dec_stride * out_alloc_h; i++) {
            uint8_t g = raw_out[i];
            rgb565[i] = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
        }
        free(raw_out);
        *out_pixels = rgb565;
    } else {
        *out_pixels = (uint16_t *)raw_out;
    }

    *out_w = (int)info.width;
    *out_h = (int)info.height;
    *out_stride = dec_stride;
    return true;
}

static void scale_crop_rgb565(const uint16_t *src, int src_w, int src_h, int src_stride,
                              uint16_t *dst, int dst_w, int dst_h)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || src_stride < src_w || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = src_w;
    int crop_h = src_h;

    if ((int64_t)src_w * dst_h > (int64_t)src_h * dst_w) {
        crop_w = (int)((int64_t)src_h * dst_w / dst_h);
        crop_x = (src_w - crop_w) / 2;
    } else if ((int64_t)src_w * dst_h < (int64_t)src_h * dst_w) {
        crop_h = (int)((int64_t)src_w * dst_h / dst_w);
        crop_y = (src_h - crop_h) / 2;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = crop_y + (int)((int64_t)y * crop_h / dst_h);
        const uint16_t *src_row = src + (size_t)sy * src_stride;
        uint16_t *dst_row = dst + (size_t)y * dst_w;
        for (int x = 0; x < dst_w; x++) {
            int sx = crop_x + (int)((int64_t)x * crop_w / dst_w);
            dst_row[x] = src_row[sx];
        }
    }
}

static esp_err_t init_display_ui(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = false,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };

    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "bsp_display_start_with_config failed");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "bsp_display_backlight_on failed");

    /* Keep UI in landscape orientation regardless of panel native orientation. */
    int hor = lv_display_get_horizontal_resolution(disp);
    int ver = lv_display_get_vertical_resolution(disp);
    if (hor < ver) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
    }

    const size_t frame_bytes = (size_t)ALBUM_ART_W * ALBUM_ART_H * 2u;
    s_album_frame = (uint16_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_album_frame) {
        s_album_frame = (uint16_t *)malloc(frame_bytes);
    }
    ESP_RETURN_ON_FALSE(s_album_frame, ESP_ERR_NO_MEM, TAG, "album frame alloc failed");
    memset(s_album_frame, 0, frame_bytes);

    memset(&s_album_dsc, 0, sizeof(s_album_dsc));
    s_album_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_album_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_album_dsc.header.flags = 0;
    s_album_dsc.header.w = ALBUM_ART_W;
    s_album_dsc.header.h = ALBUM_ART_H;
    s_album_dsc.header.stride = ALBUM_ART_W * 2;
    s_album_dsc.data_size = frame_bytes;
    s_album_dsc.data = (const uint8_t *)s_album_frame;

    ESP_RETURN_ON_FALSE(bsp_display_lock(0), ESP_FAIL, TAG, "bsp_display_lock failed");
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_album_img = lv_img_create(scr);
    lv_img_set_src(s_album_img, &s_album_dsc);
    lv_obj_set_size(s_album_img, ALBUM_ART_W, ALBUM_ART_H);
    lv_obj_center(s_album_img);
    bsp_display_unlock();

    (void)bsp_display_brightness_set(100);
    return ESP_OK;
}

static void album_task(void *arg)
{
    (void)arg;
    const TickType_t wait_tick = pdMS_TO_TICKS(3000);

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
                               s_ev,
                               EV_USB_ATTACHED | EV_USB_NETIF_READY,
                               pdFALSE,
                               pdTRUE,
                               wait_tick);

        if ((bits & (EV_USB_ATTACHED | EV_USB_NETIF_READY)) != (EV_USB_ATTACHED | EV_USB_NETIF_READY)) {
            ESP_LOGI(TAG, "Waiting for USB attach/netif...");
            continue;
        }

        if (!usb_network_ready()) {
            ESP_LOGW(TAG, "USB network not ready");
            continue;
        }

        uint8_t *json = NULL;
        size_t json_len = 0;
        esp_err_t err = http_get_alloc(YTMD_URL_API, MAX_JSON_BYTES, HTTP_TIMEOUT_MS, true, &json, &json_len);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }

        char *art_url = extract_art_url_from_song_json(json, json_len);
        char *video_id = extract_video_id_from_song_json(json, json_len);
        free(json);
        json = NULL;

        if ((!art_url || art_url[0] == '\0') && video_id) {
            char fb_url[ART_URL_MAX_LEN] = {0};
            if (make_ytimg_fallback_url(video_id, fb_url, sizeof(fb_url))) {
                char *new_url = dup_cstr(fb_url);
                if (new_url) {
                    if (art_url) {
                        free(art_url);
                    }
                    art_url = new_url;
                }
            }
        }

        if (!art_url || art_url[0] == '\0') {
            free(art_url);
            free(video_id);
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }

        if (strncmp(art_url, s_last_art_url, sizeof(s_last_art_url) - 1) == 0) {
            free(art_url);
            free(video_id);
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }

        char fb_url[ART_URL_MAX_LEN] = {0};
        bool has_fallback = make_ytimg_fallback_url(video_id, fb_url, sizeof(fb_url));
        bool using_fallback_url = has_fallback && (strcmp(art_url, fb_url) == 0);
        ESP_LOGI(TAG, "Art URL selected=%s videoId=%s fallback=%s",
                 art_url,
                 video_id ? video_id : "(none)",
                 has_fallback ? fb_url : "(none)");

        uint8_t *img = NULL;
        size_t img_len = 0;
        err = http_get_alloc(art_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, &img, &img_len);
        if (err != ESP_OK && has_fallback && !using_fallback_url) {
            ESP_LOGW(TAG, "Primary art URL failed, trying fallback: %s", fb_url);
            err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, &img, &img_len);
            if (err == ESP_OK) {
                char *new_url = dup_cstr(fb_url);
                if (new_url) {
                    free(art_url);
                    art_url = new_url;
                    using_fallback_url = true;
                }
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Art download failed: %s", art_url);
            free(art_url);
            free(video_id);
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }

        if (!is_jpeg_data(img, img_len)) {
            ESP_LOGW(TAG, "Art payload is not JPEG (url=%s)", art_url);
            if (has_fallback && !using_fallback_url) {
                ESP_LOGW(TAG, "Primary art is not JPEG, retry with fallback: %s", fb_url);
                free(img);
                img = NULL;
                img_len = 0;
                err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, &img, &img_len);
                if (err == ESP_OK) {
                    char *new_url = dup_cstr(fb_url);
                    if (new_url) {
                        free(art_url);
                        art_url = new_url;
                        using_fallback_url = true;
                    }
                }
            }
            if (!img || !is_jpeg_data(img, img_len)) {
                ESP_LOGW(TAG, "No JPEG payload available after fallback");
                free(img);
                free(art_url);
                free(video_id);
                vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                continue;
            }
        }

        uint16_t *decoded = NULL;
        int w = 0;
        int h = 0;
        int stride = 0;
        bool ok = decode_jpeg_rgb565(img, img_len, &decoded, &w, &h, &stride);
        if (!ok && has_fallback && !using_fallback_url) {
            ESP_LOGW(TAG, "JPEG decode failed on primary URL, retry fallback: %s", fb_url);
            free(img);
            img = NULL;
            img_len = 0;
            err = http_get_alloc(fb_url, MAX_IMAGE_BYTES, HTTP_TIMEOUT_MS, false, &img, &img_len);
            if (err == ESP_OK) {
                char *new_url = dup_cstr(fb_url);
                if (new_url) {
                    free(art_url);
                    art_url = new_url;
                    using_fallback_url = true;
                }
                ok = decode_jpeg_rgb565(img, img_len, &decoded, &w, &h, &stride);
            }
        }
        free(img);
        img = NULL;

        if (!ok) {
            ESP_LOGW(TAG, "JPEG decode failed: %s", art_url);
            free(art_url);
            free(video_id);
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }

        scale_crop_rgb565(decoded, w, h, stride, s_album_frame, ALBUM_ART_W, ALBUM_ART_H);
        free(decoded);

        if (bsp_display_lock(0)) {
            lv_img_set_src(s_album_img, &s_album_dsc);
            lv_obj_invalidate(s_album_img);
            bsp_display_unlock();
        }

        snprintf(s_last_art_url, sizeof(s_last_art_url), "%s", art_url);
        ESP_LOGI(TAG, "Album art updated: %dx%d <- %s", w, h, art_url);
        free(art_url);
        free(video_id);

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
#if !SOC_USB_OTG_SUPPORTED
    ESP_LOGE(TAG, "This target does not support USB OTG device mode.");
    return;
#else
    ESP_LOGI(TAG, "USB NCM + YTMD album art viewer start");

    s_ev = xEventGroupCreate();
    if (!s_ev) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_usb_ncm_and_netif());
    ESP_ERROR_CHECK(init_display_ui());
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
