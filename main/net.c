#include "net.h"

#include <string.h>

#include "dhcpserver/dhcpserver.h"
#include "dns_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"

#define RETRY_FAST_US   (3 * 1000000LL)
#define RETRY_SLOW_US   (30 * 1000000LL)
#define RETRY_FAST_TRIES 5

static const char *TAG = "net";

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static esp_timer_handle_t s_retry_timer;
static int s_retry_count;
static volatile bool s_sta_connected;
static volatile int s_ap_clients;
static volatile bool s_time_synced;
static bool s_sta_configured;
static char s_sta_ip[16];
static dashcam_settings_t s_cfg;

static void schedule_retry(void)
{
    int64_t delay = s_retry_count < RETRY_FAST_TRIES ? RETRY_FAST_US : RETRY_SLOW_US;
    s_retry_count++;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, delay);
}

static void retry_cb(void *arg)
{
    if (!s_sta_configured || s_sta_connected) return;
    /* Scanning for the home network hops channels and drops phones on our AP. */
    if (s_ap_clients > 0) {
        esp_timer_start_once(s_retry_timer, RETRY_SLOW_US);
        return;
    }
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_sta_configured) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev = data;
            if (s_sta_connected || s_retry_count < RETRY_FAST_TRIES) {
                ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d)", ev->reason);
            }
            s_sta_connected = false;
            s_sta_ip[0] = '\0';
            schedule_retry();
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            ESP_LOGI(TAG, "AP client joined (%d)", s_ap_clients);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) s_ap_clients--;
            ESP_LOGI(TAG, "AP client left (%d)", s_ap_clients);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "joined %s as %s (http://%s.local)", s_cfg.sta_ssid, s_sta_ip, s_cfg.hostname);
        esp_sntp_restart();
    }
}

static void on_time_sync(struct timeval *tv)
{
    s_time_synced = true;
    ESP_LOGI(TAG, "time synchronized via NTP");
}

static void configure_ap_dhcp(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);

    /* Point AP clients at our own DNS so any hostname reaches the dashcam. */
    esp_netif_dns_info_t dns = {
        .ip.u_addr.ip4.addr = ip.ip.addr,
        .ip.type = ESP_IPADDR_TYPE_V4,
    };
    dhcps_offer_t offer_dns = OFFER_DNS;
    static char portal_uri[32];
    snprintf(portal_uri, sizeof(portal_uri), "http://" IPSTR "/", IP2STR(&ip.ip));

    esp_netif_dhcps_stop(s_ap);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, portal_uri, strlen(portal_uri));
    esp_netif_dhcps_start(s_ap);
    dns_server_start(ip.ip.addr);
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(s_cfg.hostname);
    mdns_instance_name_set("XIAO Dashcam");
    mdns_service_add("XIAO Dashcam", "_http", "_tcp", 80, NULL, 0);
}

esp_err_t net_start(const dashcam_settings_t *settings)
{
    s_cfg = *settings;
    s_sta_configured = s_cfg.sta_ssid[0] != '\0';

    ESP_ERROR_CHECK(esp_netif_init());
    s_ap = esp_netif_create_default_wifi_ap();
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, s_cfg.hostname);
    esp_netif_set_hostname(s_ap, s_cfg.hostname);

    const esp_timer_create_args_t timer_args = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_sta_configured ? WIFI_MODE_APSTA : WIFI_MODE_AP));

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_cfg.ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_cfg.ap_ssid);
    strlcpy((char *)ap.ap.password, s_cfg.ap_pass, sizeof(ap.ap.password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = s_cfg.ap_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    if (s_sta_configured) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, s_cfg.sta_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, s_cfg.sta_pass, sizeof(sta.sta.password));
        sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }

    configure_ap_dhcp();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = on_time_sync;
    esp_netif_sntp_init(&sntp);

    start_mdns();

    ESP_LOGI(TAG, "AP \"%s\" (%s) at http://192.168.4.1  hostname %s.local", s_cfg.ap_ssid,
             s_cfg.ap_pass[0] ? "WPA2" : "open", s_cfg.hostname);
    return ESP_OK;
}

void net_get_status(net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sta_configured = s_sta_configured;
    out->sta_connected = s_sta_connected;
    strlcpy(out->sta_ssid, s_cfg.sta_ssid, sizeof(out->sta_ssid));
    strlcpy(out->sta_ip, s_sta_ip, sizeof(out->sta_ip));
    if (s_sta_connected) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) out->sta_rssi = rec.rssi;
    }
    strlcpy(out->ap_ssid, s_cfg.ap_ssid, sizeof(out->ap_ssid));
    esp_netif_ip_info_t ip;
    if (s_ap && esp_netif_get_ip_info(s_ap, &ip) == ESP_OK) {
        snprintf(out->ap_ip, sizeof(out->ap_ip), IPSTR, IP2STR(&ip.ip));
    }
    out->ap_open = s_cfg.ap_pass[0] == '\0';
    out->ap_clients = s_ap_clients;
    strlcpy(out->hostname, s_cfg.hostname, sizeof(out->hostname));
    out->time_synced = s_time_synced;
}

void net_mark_time_set(void)
{
    s_time_synced = true;
}
