#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LOGBUF_SIZE (24 * 1024)

static char *s_buf;
static size_t s_head;
static bool s_wrapped;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_console_vprintf;
static volatile bool s_console_enabled = true;

static void append(const char *src, size_t len)
{
    portENTER_CRITICAL(&s_lock);
    while (len > 0) {
        size_t chunk = LOGBUF_SIZE - s_head;
        if (chunk > len) chunk = len;
        memcpy(s_buf + s_head, src, chunk);
        s_head += chunk;
        src += chunk;
        len -= chunk;
        if (s_head == LOGBUF_SIZE) {
            s_head = 0;
            s_wrapped = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static int logbuf_vprintf(const char *fmt, va_list args)
{
    char line[200];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0 && s_buf) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        append(line, len);
    }
    if (s_console_enabled && s_console_vprintf) return s_console_vprintf(fmt, args);
    return n;
}

void logbuf_init(void)
{
    s_buf = heap_caps_malloc(LOGBUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) return;
    s_console_vprintf = esp_log_set_vprintf(logbuf_vprintf);
}

void logbuf_set_console(bool enabled)
{
    s_console_enabled = enabled;
}

size_t logbuf_read(char *dst, size_t cap)
{
    if (!s_buf || cap == 0) return 0;
    size_t total;
    portENTER_CRITICAL(&s_lock);
    if (!s_wrapped) {
        total = s_head < cap ? s_head : cap;
        memcpy(dst, s_buf + (s_head - total), total);
    } else {
        total = LOGBUF_SIZE < cap ? LOGBUF_SIZE : cap;
        /* Copy the newest `total` bytes in chronological order. */
        size_t start = (s_head + LOGBUF_SIZE - total) % LOGBUF_SIZE;
        size_t first = LOGBUF_SIZE - start;
        if (first > total) first = total;
        memcpy(dst, s_buf + start, first);
        memcpy(dst + first, s_buf, total - first);
    }
    portEXIT_CRITICAL(&s_lock);
    return total;
}
