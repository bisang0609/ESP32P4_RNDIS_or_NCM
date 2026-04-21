/*
 * Minimal USB-NCM -> HTTP test skeleton for ESP32-P4 (ESP-IDF).
 *
 * Goal:
 * 1) Enumerate as USB network device on Windows host
 * 2) Bring up local USB netif on ESP
 * 3) Send HTTP GET to YTMD API running on host (example: 192.168.7.1:26538)
 *
 * NOTE:
 * - This is intentionally a test skeleton with verbose logs and explicit TODOs.
 * - For quickest bring-up, this uses STATIC IP (no DHCP server).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "soc/soc_caps.h"

#include "lwip/esp_netif_net_stack.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

static const char *TAG = "USB_NCM_YTMD";

#define EV_USB_ATTACHED      BIT0
#define EV_USB_NETIF_READY   BIT1

/* Test network plan:
 *  - Windows PC static IP: 192.168.7.1/24
 *  - ESP board static IP : 192.168.7.2/24
 */
#define USB_LOCAL_IP_STR     "192.168.7.2"
#define USB_GW_IP_STR        "192.168.7.1"
#define USB_NETMASK_STR      "255.255.255.0"

/* HTTP targets */
#define YTMD_URL_ROOT        "http://192.168.7.1:26538/api/v1/song"
#define YTMD_URL_API         "http://192.168.7.1:26538/api/v1/song"

static EventGroupHandle_t s_ev = NULL;
static esp_netif_t *s_usb_netif = NULL;

static void tinyusb_event_handler(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (!s_ev || !event) {
        return;
    }

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "TinyUSB event: ATTACHED (enumerated by host)");
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
        ESP_LOGE(TAG, "TX too large for USB NET: %u", (unsigned)len);
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

    /* ethernetif_input() expects driver-owned RX buffer and frees it later
     * through driver_free_rx_buffer callback.
     */
    void *buf_copy = malloc(len);
    if (!buf_copy) {
        ESP_LOGE(TAG, "RX malloc failed (%u bytes)", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf_copy, buffer, len);

    esp_err_t err = esp_netif_receive(s_usb_netif, buf_copy, len, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_receive failed: %s", esp_err_to_name(err));
        free(buf_copy);
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
    ESP_RETURN_ON_ERROR(parse_static_ip(&ip_info), TAG, "parse_static_ip failed");

    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_stop: %s", esp_err_to_name(err));
    }

    err = esp_netif_set_ip_info(netif, &ip_info);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_netif_set_ip_info failed");

    ESP_LOGI(TAG, "USB netif static IP set: ip=" IPSTR " gw=" IPSTR " mask=" IPSTR,
             IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));
    return ESP_OK;
}

static esp_err_t init_usb_ncm_and_netif(void)
{
    ESP_LOGI(TAG, "Installing TinyUSB driver...");
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tinyusb_event_handler);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install failed");

    tinyusb_net_config_t net_cfg = {
        /* Use a locally administered MAC for USB-NCM device interface */
        .mac_addr = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01},
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer = NULL,   /* Not used for sync TX path in this skeleton */
        .user_context = NULL,
    };

    ESP_LOGI(TAG, "Initializing TinyUSB NCM class...");
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&net_cfg), TAG, "tinyusb_net_init failed");
    ESP_LOGI(TAG, "USB-NCM MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             net_cfg.mac_addr[0], net_cfg.mac_addr[1], net_cfg.mac_addr[2],
             net_cfg.mac_addr[3], net_cfg.mac_addr[4], net_cfg.mac_addr[5]);

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_FLAG_AUTOUP | ESP_NETIF_FLAG_GARP,
        .if_key = "usb_ncm0",
        .if_desc = "usb_ncm_test",
        .route_prio = 10,
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1, /* singleton handle, must be non-NULL */
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

    ESP_LOGI(TAG, "Creating esp_netif for USB-NCM...");
    s_usb_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_usb_netif, ESP_ERR_NO_MEM, TAG, "esp_netif_new failed");

    /* This MAC must differ from NCM class MAC in dual-MAC model */
    uint8_t lwip_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x02};
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_usb_netif, lwip_mac), TAG, "esp_netif_set_mac failed");
    ESP_LOGI(TAG, "USB lwIP MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             lwip_mac[0], lwip_mac[1], lwip_mac[2], lwip_mac[3], lwip_mac[4], lwip_mac[5]);

    ESP_RETURN_ON_ERROR(configure_usb_netif_ip(s_usb_netif), TAG, "configure_usb_netif_ip failed");

    /* Driver is already started, explicitly mark interface started */
    esp_netif_action_start(s_usb_netif, 0, 0, 0);
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
    if (ip_info.ip.addr == 0) {
        return false;
    }
    return true;
}

static esp_err_t http_get_and_log(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "HTTP GET -> %s", url);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d content_length=%lld", status_code, (long long)content_length);

    char buf[256];
    while (1) {
        int read_len = esp_http_client_read(client, buf, sizeof(buf) - 1);
        if (read_len < 0) {
            ESP_LOGE(TAG, "esp_http_client_read failed");
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;
        }
        buf[read_len] = '\0';
        ESP_LOGI(TAG, "HTTP body chunk (%dB): %s", read_len, buf);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static void http_test_task(void *arg)
{
    (void)arg;

    const TickType_t retry_tick = pdMS_TO_TICKS(5000);
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
                               s_ev,
                               EV_USB_ATTACHED | EV_USB_NETIF_READY,
                               pdFALSE,
                               pdTRUE,
                               retry_tick);

        if ((bits & (EV_USB_ATTACHED | EV_USB_NETIF_READY)) != (EV_USB_ATTACHED | EV_USB_NETIF_READY)) {
            ESP_LOGI(TAG, "Waiting USB attach/netif ready...");
            continue;
        }

        if (!usb_network_ready()) {
            ESP_LOGW(TAG, "USB network not ready yet (no link/up/ip)");
            continue;
        }

        ESP_LOGI(TAG, "USB network ready. Start YTMD API connectivity test.");

        (void)http_get_and_log(YTMD_URL_ROOT);
        (void)http_get_and_log(YTMD_URL_API);

        /* periodic test */
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
#if !SOC_USB_OTG_SUPPORTED
    ESP_LOGE(TAG, "This target does not support USB OTG device mode.");
    return;
#else
    ESP_LOGI(TAG, "USB NCM + YTMD HTTP test start");
    ESP_LOGI(TAG, "TODO: Confirm JC4880P433 USB data port wiring and USB cable path to ESP32-P4 USB peripheral.");
    ESP_LOGI(TAG, "TODO: If USB enumeration is unstable on P4 HS port, verify PHY/VBUS config in menuconfig.");

    s_ev = xEventGroupCreate();
    if (!s_ev) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_usb_ncm_and_netif());

    BaseType_t ok = xTaskCreate(
                        http_test_task,
                        "http_test_task",
                        6144,
                        NULL,
                        5,
                        NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create http_test_task");
        return;
    }
#endif
}
