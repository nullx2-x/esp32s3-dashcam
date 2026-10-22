#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define NVS_NAMESPACE "dashcam"
#define NVS_KEY       "cfg"
#define NVS_VER_KEY   "cfg_ver"
#define SETTINGS_VERSION 1

static const char *TAG = "settings";
static dashcam_settings_t s_settings;
static SemaphoreHandle_t s_lock;

static void load_defaults(dashcam_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s->ap_ssid, sizeof(s->ap_ssid), "%s-%02X%02X", CONFIG_DASHCAM_HOSTNAME, mac[4], mac[5]);
    strlcpy(s->ap_pass, CONFIG_DASHCAM_AP_PASSWORD, sizeof(s->ap_pass));
    strlcpy(s->sta_ssid, CONFIG_DASHCAM_WIFI_SSID, sizeof(s->sta_ssid));
    strlcpy(s->sta_pass, CONFIG_DASHCAM_WIFI_PASSWORD, sizeof(s->sta_pass));
    strlcpy(s->hostname, CONFIG_DASHCAM_HOSTNAME, sizeof(s->hostname));
    strlcpy(s->tz, "JST-9", sizeof(s->tz));
    s->framesize = FRAMESIZE_HD;
    s->jpeg_quality = 12;
    s->fps_limit = 15;
    s->min_free_pct = 10;
    s->segment_sec = 60;
    s->vflip = false;
    s->hmirror = false;
    s->auto_record = true;
}

static bool valid_hostname(const char *h)
{
    if (!h[0]) return false;
    for (const char *p = h; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
    }
    return h[0] != '-';
}

void settings_sanitize(dashcam_settings_t *s)
{
    s->sta_ssid[sizeof(s->sta_ssid) - 1] = '\0';
    s->sta_pass[sizeof(s->sta_pass) - 1] = '\0';
    s->ap_ssid[sizeof(s->ap_ssid) - 1] = '\0';
    s->ap_pass[sizeof(s->ap_pass) - 1] = '\0';
    s->hostname[sizeof(s->hostname) - 1] = '\0';
    s->tz[sizeof(s->tz) - 1] = '\0';
    if (!valid_hostname(s->hostname)) strlcpy(s->hostname, CONFIG_DASHCAM_HOSTNAME, sizeof(s->hostname));
    if (!s->ap_ssid[0]) snprintf(s->ap_ssid, sizeof(s->ap_ssid), "%s", s->hostname);
    /* WPA2 needs 8+ characters; anything shorter means an open AP. */
    if (strlen(s->ap_pass) < 8) s->ap_pass[0] = '\0';
    if (!s->tz[0]) strlcpy(s->tz, "JST-9", sizeof(s->tz));
    if (s->framesize > FRAMESIZE_QXGA) s->framesize = FRAMESIZE_HD;
    if (s->jpeg_quality < 4) s->jpeg_quality = 4;
    if (s->jpeg_quality > 63) s->jpeg_quality = 63;
    if (s->fps_limit < 1) s->fps_limit = 1;
    if (s->fps_limit > 30) s->fps_limit = 30;
    if (s->min_free_pct < 2) s->min_free_pct = 2;
    if (s->min_free_pct > 50) s->min_free_pct = 50;
    if (s->segment_sec < 15) s->segment_sec = 15;
    if (s->segment_sec > 900) s->segment_sec = 900;
}

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    load_defaults(&s_settings);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        settings_sanitize(&s_settings);
        return;
    }
    uint32_t ver = 0;
    dashcam_settings_t stored;
    size_t len = sizeof(stored);
    if (nvs_get_u32(nvs, NVS_VER_KEY, &ver) == ESP_OK && ver == SETTINGS_VERSION &&
        nvs_get_blob(nvs, NVS_KEY, &stored, &len) == ESP_OK && len == sizeof(stored)) {
        s_settings = stored;
        ESP_LOGI(TAG, "loaded saved settings");
    } else {
        ESP_LOGW(TAG, "saved settings missing or outdated, using defaults");
    }
    nvs_close(nvs);
    settings_sanitize(&s_settings);
}

void settings_get(dashcam_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_save(const dashcam_settings_t *in)
{
    dashcam_settings_t copy = *in;
    settings_sanitize(&copy);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, NVS_KEY, &copy, sizeof(copy));
    if (err == ESP_OK) err = nvs_set_u32(nvs, NVS_VER_KEY, SETTINGS_VERSION);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings = copy;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
