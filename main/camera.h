#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "settings.h"

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    uint16_t width;
    uint16_t height;
    int64_t timestamp_us;
    uint32_t seq;
    int refs;
} frame_t;

esp_err_t camera_start(const dashcam_settings_t *settings);
void camera_apply_settings(const dashcam_settings_t *settings);

/* Returns a frame newer than *last_seq (and updates it), or NULL on timeout.
 * Every acquired frame must be handed back with camera_release(). */
frame_t *camera_acquire(uint32_t *last_seq, uint32_t timeout_ms);
void camera_release(frame_t *frame);

bool camera_ok(void);
float camera_fps(void);
const char *camera_sensor_name(void);
