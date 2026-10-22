#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool recording;
    char current_file[64];
    uint32_t current_frames;
    uint64_t current_bytes;
    float write_fps;
    uint32_t files_deleted;
    char last_error[96];
} recorder_status_t;

esp_err_t recorder_start(bool enabled);
void recorder_set_enabled(bool enabled);
void recorder_get_status(recorder_status_t *out);
/* Finishes the current segment so the card can be safely powered off/updated. */
void recorder_stop_and_wait(void);
void recorder_resume(void);
/* Starts a new segment right away and protects the one just finished. */
void recorder_protect_current(void);
