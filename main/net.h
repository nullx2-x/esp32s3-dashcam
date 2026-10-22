#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "settings.h"

typedef struct {
    bool sta_configured;
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    int sta_rssi;
    char ap_ssid[33];
    char ap_ip[16];
    bool ap_open;
    int ap_clients;
    char hostname[32];
    bool time_synced;
} net_status_t;

esp_err_t net_start(const dashcam_settings_t *settings);
void net_get_status(net_status_t *out);
void net_mark_time_set(void);
