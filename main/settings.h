#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char sta_ssid[33];
    char sta_pass[65];
    char ap_ssid[33];
    char ap_pass[65];
    char hostname[32];
    char tz[48];            /* POSIX TZ string, e.g. "JST-9" */
    uint8_t framesize;      /* framesize_t */
    uint8_t jpeg_quality;   /* 4 (best) .. 63 */
    uint8_t fps_limit;      /* recording frame rate cap */
    uint8_t min_free_pct;   /* loop recording keeps at least this much free */
    uint16_t segment_sec;
    bool vflip;
    bool hmirror;
    bool auto_record;
} dashcam_settings_t;

void settings_init(void);
void settings_get(dashcam_settings_t *out);
esp_err_t settings_save(const dashcam_settings_t *in);
void settings_sanitize(dashcam_settings_t *s);
