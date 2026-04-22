#include "ncm_net.h"

#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "lwip/esp_netif_net_stack.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

static const char *TAG = "USB_NCM_YTMD_ART";

#define EV_USB_ATTACHED      BIT0
#define EV_USB_NETIF_READY   BIT1

#define USB_LOCAL_IP_STR     "192.168.137.2"
#define USB_GW_IP_STR        "192.168.137.1"
#define USB_NETMASK_STR      "255.255.255.0"
#define USB_DNS_MAIN_STR     "8.8.8.8"
#define USB_DNS_BACKUP_STR   "1.1.1.1"

static EventGroupHandle_t s_ev = NULL;
static esp_netif_t *s_usb_netif = NULL;
static TickType_t s_last_net_diag_tick = 0;

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

esp_err_t ncm_net_init(void)
{
    if (!s_ev) {
        s_ev = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_ev, ESP_ERR_NO_MEM, TAG, "xEventGroupCreate failed");
    }

    if (s_usb_netif) {
        ESP_LOGI(TAG, "USB NCM netif already initialized");
        return ESP_OK;
    }

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

bool ncm_net_wait_ready(TickType_t timeout_ticks)
{
    if (!s_ev) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_ev,
        EV_USB_ATTACHED | EV_USB_NETIF_READY,
        pdFALSE,
        pdTRUE,
        timeout_ticks);

    return (bits & (EV_USB_ATTACHED | EV_USB_NETIF_READY)) == (EV_USB_ATTACHED | EV_USB_NETIF_READY);
}

bool ncm_net_is_ready(void)
{
    if (!s_ev) {
        return false;
    }

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

void ncm_net_log_diagnostics(const char *url)
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
