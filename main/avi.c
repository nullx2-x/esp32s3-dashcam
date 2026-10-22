#include "avi.h"

#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/* SD over SPI only does multi-block transfers from DMA-capable memory, so all
 * file I/O goes through internal buffers at aligned file offsets. */
#define WRITE_BUF_SIZE (32 * 1024)
#define READ_BUF_SIZE  (16 * 1024)

#define HEADER_SIZE    224
#define MOVI_FOURCC_AT 220
#define AVIF_HASINDEX  0x10
#define AVIIF_KEYFRAME 0x10

static const char *TAG = "avi";

typedef struct {
    uint32_t offset;
    uint32_t size;
} index_entry_t;

struct avi_writer {
    int fd;
    uint8_t *buf;
    size_t buf_used;
    uint64_t flushed;
    uint32_t frames;
    uint32_t max_frame;
    index_entry_t *index;
    size_t index_cap;
    int64_t first_ts;
    int64_t last_ts;
    uint16_t width;
    uint16_t height;
    uint32_t nominal_fps;
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static double measured_fps(const avi_writer_t *w)
{
    if (w->frames >= 2 && w->last_ts > w->first_ts) {
        return (w->frames - 1) * 1e6 / (double)(w->last_ts - w->first_ts);
    }
    return w->nominal_fps;
}

static void build_header(const avi_writer_t *w, uint8_t *h, uint32_t riff_size, uint32_t movi_size,
                         uint32_t frames, bool indexed)
{
    double fps = measured_fps(w);
    if (fps < 0.1) fps = 0.1;

    memset(h, 0, HEADER_SIZE);
    memcpy(h + 0, "RIFF", 4);
    put32(h + 4, riff_size);
    memcpy(h + 8, "AVI ", 4);

    memcpy(h + 12, "LIST", 4);
    put32(h + 16, 192);
    memcpy(h + 20, "hdrl", 4);

    memcpy(h + 24, "avih", 4);
    put32(h + 28, 56);
    put32(h + 32, (uint32_t)(1e6 / fps));
    put32(h + 36, (uint32_t)(w->max_frame * fps));
    put32(h + 44, indexed ? AVIF_HASINDEX : 0);
    put32(h + 48, frames);
    put32(h + 56, 1);
    put32(h + 60, w->max_frame + 8);
    put32(h + 64, w->width);
    put32(h + 68, w->height);

    memcpy(h + 88, "LIST", 4);
    put32(h + 92, 116);
    memcpy(h + 96, "strl", 4);

    memcpy(h + 100, "strh", 4);
    put32(h + 104, 56);
    memcpy(h + 108, "vids", 4);
    memcpy(h + 112, "MJPG", 4);
    put32(h + 128, 1000);
    put32(h + 132, (uint32_t)(fps * 1000 + 0.5));
    put32(h + 140, frames);
    put32(h + 144, w->max_frame + 8);
    put32(h + 148, 0xFFFFFFFF);
    put16(h + 160, w->width);
    put16(h + 162, w->height);

    memcpy(h + 164, "strf", 4);
    put32(h + 168, 40);
    put32(h + 172, 40);
    put32(h + 176, w->width);
    put32(h + 180, w->height);
    put16(h + 184, 1);
    put16(h + 186, 24);
    memcpy(h + 188, "MJPG", 4);
    put32(h + 192, (uint32_t)w->width * w->height * 3);

    memcpy(h + 212, "LIST", 4);
    put32(h + 216, movi_size);
    memcpy(h + 220, "movi", 4);
}

static esp_err_t write_all(int fd, const uint8_t *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) return ESP_FAIL;
        data += n;
        len -= n;
    }
    return ESP_OK;
}

static esp_err_t flush_full_buffer(avi_writer_t *w)
{
    esp_err_t err = write_all(w->fd, w->buf, w->buf_used);
    if (err != ESP_OK) return err;
    w->flushed += w->buf_used;
    w->buf_used = 0;
    return ESP_OK;
}

static esp_err_t append(avi_writer_t *w, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t space = WRITE_BUF_SIZE - w->buf_used;
        size_t chunk = len < space ? len : space;
        memcpy(w->buf + w->buf_used, data, chunk);
        w->buf_used += chunk;
        data += chunk;
        len -= chunk;
        if (w->buf_used == WRITE_BUF_SIZE) {
            esp_err_t err = flush_full_buffer(w);
            if (err != ESP_OK) return err;
        }
    }
    return ESP_OK;
}

static uint64_t stream_pos(const avi_writer_t *w)
{
    return w->flushed + w->buf_used;
}

static esp_err_t write_header(avi_writer_t *w, uint32_t riff_size, uint32_t movi_size, uint32_t frames,
                              bool indexed)
{
    uint8_t header[HEADER_SIZE];
    build_header(w, header, riff_size, movi_size, frames, indexed);
    if (w->flushed == 0) {
        memcpy(w->buf, header, HEADER_SIZE);
        return ESP_OK;
    }
    if (lseek(w->fd, 0, SEEK_SET) != 0) return ESP_FAIL;
    esp_err_t err = write_all(w->fd, header, HEADER_SIZE);
    if (lseek(w->fd, (off_t)w->flushed, SEEK_SET) != (off_t)w->flushed) return ESP_FAIL;
    return err;
}

avi_writer_t *avi_writer_open(const char *path, uint16_t width, uint16_t height, uint32_t nominal_fps)
{
    avi_writer_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM);
    if (!w) return NULL;
    w->buf = heap_caps_malloc(WRITE_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    w->index_cap = 2048;
    w->index = heap_caps_malloc(w->index_cap * sizeof(index_entry_t), MALLOC_CAP_SPIRAM);
    if (!w->buf || !w->index) goto fail;

    w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (w->fd < 0) {
        ESP_LOGE(TAG, "open %s failed", path);
        goto fail;
    }
    w->width = width;
    w->height = height;
    w->nominal_fps = nominal_fps ? nominal_fps : 10;
    w->first_ts = -1;

    uint8_t header[HEADER_SIZE];
    build_header(w, header, HEADER_SIZE - 8, 4, 0, false);
    append(w, header, HEADER_SIZE);
    return w;

fail:
    heap_caps_free(w->buf);
    heap_caps_free(w->index);
    heap_caps_free(w);
    return NULL;
}

esp_err_t avi_writer_add_frame(avi_writer_t *w, const uint8_t *jpeg, size_t len, int64_t timestamp_us)
{
    if (w->frames == w->index_cap) {
        size_t cap = w->index_cap * 2;
        index_entry_t *grown = heap_caps_realloc(w->index, cap * sizeof(index_entry_t), MALLOC_CAP_SPIRAM);
        if (!grown) return ESP_ERR_NO_MEM;
        w->index = grown;
        w->index_cap = cap;
    }

    uint8_t chunk_header[8];
    memcpy(chunk_header, "00dc", 4);
    put32(chunk_header + 4, len);
    w->index[w->frames].offset = (uint32_t)(stream_pos(w) - MOVI_FOURCC_AT);
    w->index[w->frames].size = len;

    esp_err_t err = append(w, chunk_header, sizeof(chunk_header));
    if (err == ESP_OK) err = append(w, jpeg, len);
    if (err == ESP_OK && (len & 1)) {
        const uint8_t pad = 0;
        err = append(w, &pad, 1);
    }
    if (err != ESP_OK) return err;

    if (w->first_ts < 0) w->first_ts = timestamp_us;
    w->last_ts = timestamp_us;
    if (len > w->max_frame) w->max_frame = len;
    w->frames++;
    return ESP_OK;
}

esp_err_t avi_writer_sync(avi_writer_t *w)
{
    if (w->flushed == 0) return ESP_OK;
    /* Only describe what is already on the card. */
    uint32_t frames_on_disk = 0;
    for (uint32_t i = w->frames; i > 0; i--) {
        const index_entry_t *e = &w->index[i - 1];
        if (MOVI_FOURCC_AT + (uint64_t)e->offset + 8 + e->size <= w->flushed) {
            frames_on_disk = i;
            break;
        }
    }
    esp_err_t err = write_header(w, (uint32_t)(w->flushed - 8), (uint32_t)(w->flushed - MOVI_FOURCC_AT),
                                 frames_on_disk, false);
    if (err == ESP_OK && fsync(w->fd) != 0) err = ESP_FAIL;
    return err;
}

esp_err_t avi_writer_close(avi_writer_t *w)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    const uint64_t movi_end = stream_pos(w);

    uint8_t idx_header[8];
    memcpy(idx_header, "idx1", 4);
    put32(idx_header + 4, w->frames * 16);
    err = append(w, idx_header, sizeof(idx_header));
    for (uint32_t i = 0; err == ESP_OK && i < w->frames; i++) {
        uint8_t entry[16];
        memcpy(entry, "00dc", 4);
        put32(entry + 4, AVIIF_KEYFRAME);
        put32(entry + 8, w->index[i].offset);
        put32(entry + 12, w->index[i].size);
        err = append(w, entry, sizeof(entry));
    }
    if (err == ESP_OK && w->buf_used > 0) err = flush_full_buffer(w);
    if (err == ESP_OK) {
        err = write_header(w, (uint32_t)(w->flushed - 8), (uint32_t)(movi_end - MOVI_FOURCC_AT), w->frames, true);
    }
    if (fsync(w->fd) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (close(w->fd) != 0 && err == ESP_OK) err = ESP_FAIL;

    heap_caps_free(w->buf);
    heap_caps_free(w->index);
    heap_caps_free(w);
    return err;
}

uint32_t avi_writer_frames(const avi_writer_t *w)
{
    return w ? w->frames : 0;
}

uint64_t avi_writer_bytes(const avi_writer_t *w)
{
    return w ? stream_pos(w) : 0;
}

struct avi_reader {
    int fd;
    uint8_t *buf;
    size_t buf_len;
    size_t buf_pos;
    bool eof;
};

static bool reader_fill(avi_reader_t *r)
{
    if (r->eof) return false;
    ssize_t n = read(r->fd, r->buf, READ_BUF_SIZE);
    if (n <= 0) {
        r->eof = true;
        return false;
    }
    r->buf_len = n;
    r->buf_pos = 0;
    return true;
}

static bool reader_read(avi_reader_t *r, uint8_t *dst, size_t len)
{
    while (len > 0) {
        if (r->buf_pos == r->buf_len && !reader_fill(r)) return false;
        size_t avail = r->buf_len - r->buf_pos;
        size_t chunk = len < avail ? len : avail;
        if (dst) {
            memcpy(dst, r->buf + r->buf_pos, chunk);
            dst += chunk;
        }
        r->buf_pos += chunk;
        len -= chunk;
    }
    return true;
}

avi_reader_t *avi_reader_open(const char *path, uint32_t *us_per_frame, uint16_t *width, uint16_t *height)
{
    avi_reader_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM);
    if (!r) return NULL;
    r->buf = heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    r->fd = r->buf ? open(path, O_RDONLY) : -1;
    if (r->fd < 0) goto fail;

    uint8_t riff[12];
    if (!reader_read(r, riff, sizeof(riff)) || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "AVI ", 4)) {
        close(r->fd);
        goto fail;
    }
    *us_per_frame = 100000;
    *width = 0;
    *height = 0;

    /* Walk chunks until the first frame is reached, picking up avih on the way. */
    for (;;) {
        uint8_t ch[8];
        if (!reader_read(r, ch, sizeof(ch))) break;
        uint32_t size = get32(ch + 4);
        if (!memcmp(ch, "LIST", 4)) {
            uint8_t type[4];
            if (!reader_read(r, type, 4)) break;
            if (!memcmp(type, "movi", 4)) return r;
            continue;
        }
        if (!memcmp(ch, "avih", 4) && size >= 40) {
            uint8_t avih[56] = {0};
            size_t take = size < sizeof(avih) ? size : sizeof(avih);
            if (!reader_read(r, avih, take) || !reader_read(r, NULL, size - take + (size & 1))) break;
            if (get32(avih)) *us_per_frame = get32(avih);
            *width = get32(avih + 32);
            *height = get32(avih + 36);
            continue;
        }
        if (!reader_read(r, NULL, size + (size & 1))) break;
    }
    close(r->fd);
fail:
    heap_caps_free(r->buf);
    heap_caps_free(r);
    return NULL;
}

int avi_reader_next(avi_reader_t *r, uint8_t *buf, size_t cap)
{
    for (;;) {
        uint8_t ch[8];
        if (!reader_read(r, ch, sizeof(ch))) return 0;
        uint32_t size = get32(ch + 4);
        if (!memcmp(ch, "idx1", 4)) return 0;
        if (!memcmp(ch, "LIST", 4)) {
            if (!reader_read(r, NULL, 4)) return 0;
            continue;
        }
        if (ch[2] == 'd' && (ch[3] == 'c' || ch[3] == 'b')) {
            if (size > cap) {
                if (!reader_read(r, NULL, size + (size & 1))) return 0;
                continue;
            }
            if (!reader_read(r, buf, size)) return 0;
            if (size & 1) reader_read(r, NULL, 1);
            return (int)size;
        }
        if (!reader_read(r, NULL, size + (size & 1))) return 0;
    }
}

void avi_reader_close(avi_reader_t *r)
{
    if (!r) return;
    close(r->fd);
    heap_caps_free(r->buf);
    heap_caps_free(r);
}
