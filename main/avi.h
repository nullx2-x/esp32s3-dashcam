#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct avi_writer avi_writer_t;

avi_writer_t *avi_writer_open(const char *path, uint16_t width, uint16_t height, uint32_t nominal_fps);
esp_err_t avi_writer_add_frame(avi_writer_t *w, const uint8_t *jpeg, size_t len, int64_t timestamp_us);
/* Makes everything written so far playable after a sudden power loss. */
esp_err_t avi_writer_sync(avi_writer_t *w);
esp_err_t avi_writer_close(avi_writer_t *w);
uint32_t avi_writer_frames(const avi_writer_t *w);
uint64_t avi_writer_bytes(const avi_writer_t *w);

/* Sequential reader for MJPEG AVI files (used for in-browser playback). */
typedef struct avi_reader avi_reader_t;

avi_reader_t *avi_reader_open(const char *path, uint32_t *us_per_frame, uint16_t *width, uint16_t *height);
/* Returns frame length, 0 at end of file, or -1 on error. */
int avi_reader_next(avi_reader_t *r, uint8_t *buf, size_t cap);
void avi_reader_close(avi_reader_t *r);
