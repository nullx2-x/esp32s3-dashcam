#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Keeps the most recent log output in RAM so it can be read over HTTP while
 * the USB port is busy acting as a webcam. */
void logbuf_init(void);
void logbuf_set_console(bool enabled);
size_t logbuf_read(char *dst, size_t cap);
