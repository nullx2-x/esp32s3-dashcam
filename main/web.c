#include "web.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "avi.h"
#include "cJSON.h"
#include "camera.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "net.h"
#include "recorder.h"
#include "settings.h"
#include "storage.h"
#include "usb_uvc.h"

#define BOUNDARY        "dashcamframe"
#define MAX_ASYNC_JOBS  4
#define FILE_BUF_SIZE   (16 * 1024)
#define PLAY_BUF_SIZE   (640 * 1024)
#define LIST_BUF_SIZE   4096

#define ASSET_DIR       SD_MOUNT_POINT "/www"
#define ASSET_MAX_PATH  96
#define ASSET_MAX_DEPTH 3
#define ASSET_MAX_SIZE  (2 * 1024 * 1024)
#define ASSET_MAX_LIST  200
#define ASSET_BUF_SIZE  (8 * 1024)
/* Applies when an asset is opened as a document (e.g. an SVG), so SD content cannot run script. */
#define ASSET_CSP "default-src 'none'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;
static SemaphoreHandle_t s_async_slots;

typedef struct web_job web_job_t;
typedef void (*job_fn_t)(web_job_t *job);

struct web_job {
    httpd_req_t *req;
    job_fn_t fn;
    char path[112];
    char name[64];
    int speed;
};

static const struct {
    framesize_t size;
    const char *label;
} FRAME_SIZES[] = {
    {FRAMESIZE_VGA, "VGA 640x480"},
    {FRAMESIZE_SVGA, "SVGA 800x600"},
    {FRAMESIZE_XGA, "XGA 1024x768"},
    {FRAMESIZE_HD, "HD 1280x720"},
    {FRAMESIZE_SXGA, "SXGA 1280x1024"},
    {FRAMESIZE_UXGA, "UXGA 1600x1200"},
    {FRAMESIZE_FHD, "FHD 1920x1080"},
    {FRAMESIZE_QXGA, "QXGA 2048x1536"},
};

/* Only passive content is served from the SD card; HTML and JavaScript stay in the firmware. */
static const struct {
    const char *ext;
    const char *type;
} ASSET_TYPES[] = {
    {"css", "text/css; charset=utf-8"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
};

/* ---------- helpers ---------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    if (message) cJSON_AddStringToObject(root, "message", message);
    if (!ok) httpd_resp_set_status(req, "400 Bad Request");
    return send_json(req, root);
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

static bool safe_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= 64 || strstr(name, "..")) return false;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static bool resolve_media(const char *dir, const char *name, char *path, size_t cap)
{
    if (strcmp(dir, "REC") != 0 && strcmp(dir, "LOCK") != 0) return false;
    if (!safe_name(name)) return false;
    snprintf(path, cap, SD_MOUNT_POINT "/%s/%s", dir, name);
    return true;
}

/* "/files/REC/000001_x.avi?..." -> dir="REC", name="000001_x.avi" */
static bool parse_media_uri(const char *uri, const char *prefix, char *dir, size_t dir_cap, char *name,
                            size_t name_cap)
{
    const char *p = uri + strlen(prefix);
    const char *slash = strchr(p, '/');
    if (!slash || (size_t)(slash - p) >= dir_cap) return false;
    memcpy(dir, p, slash - p);
    dir[slash - p] = '\0';
    const char *start = slash + 1;
    size_t len = strcspn(start, "?");
    if (len == 0 || len >= name_cap) return false;
    memcpy(name, start, len);
    name[len] = '\0';
    return true;
}

static bool send_all(httpd_req_t *req, const char *data, size_t len)
{
    while (len > 0) {
        int n = httpd_send(req, data, len);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return false;
        data += n;
        len -= n;
    }
    return true;
}

static esp_err_t send_mjpeg_part(httpd_req_t *req, const uint8_t *jpeg, size_t len)
{
    char part[96];
    int n = snprintf(part, sizeof(part),
                     "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     (unsigned)len);
    esp_err_t err = httpd_resp_send_chunk(req, part, n);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)jpeg, len);
    return err;
}

/* ---------- long-running requests run on their own task ---------- */

static void async_task(void *arg)
{
    web_job_t *job = arg;
    job->fn(job);
    httpd_req_async_handler_complete(job->req);
    free(job);
    xSemaphoreGive(s_async_slots);
    vTaskDelete(NULL);
}

static esp_err_t start_async(httpd_req_t *req, web_job_t *job)
{
    if (xSemaphoreTake(s_async_slots, 0) != pdTRUE) {
        free(job);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Too many streams/downloads at once");
    }
    if (httpd_req_async_handler_begin(req, &job->req) != ESP_OK) {
        free(job);
        xSemaphoreGive(s_async_slots);
        return httpd_resp_send_500(req);
    }
    if (xTaskCreate(async_task, "http_job", 6144, job, 5, NULL) != pdPASS) {
        httpd_req_async_handler_complete(job->req);
        free(job);
        xSemaphoreGive(s_async_slots);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void stream_job(web_job_t *job)
{
    httpd_req_t *req = job->req;
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint32_t seq = 0;
    int misses = 0;
    for (;;) {
        frame_t *f = camera_acquire(&seq, 2000);
        if (!f) {
            if (++misses > 5) break;
            continue;
        }
        misses = 0;
        esp_err_t err = send_mjpeg_part(req, f->data, f->len);
        camera_release(f);
        if (err != ESP_OK) break;
    }
    httpd_resp_send_chunk(req, NULL, 0);
}

static void download_job(web_job_t *job)
{
    httpd_req_t *req = job->req;
    struct stat st;
    if (stat(job->path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return;
    }
    const uint64_t size = st.st_size;
    uint64_t start = 0, end = size ? size - 1 : 0;
    bool partial = false;
    char range[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        unsigned long long a = 0, b = 0;
        int n = sscanf(range, "bytes=%llu-%llu", &a, &b);
        if (n >= 1 && a < size) {
            start = a;
            if (n == 2 && b >= a && b < size) end = b;
            partial = true;
        }
    }
    const uint64_t len = size ? end - start + 1 : 0;

    int fd = open(job->path, O_RDONLY);
    uint8_t *buf = heap_caps_malloc(FILE_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (fd < 0 || !buf) {
        if (fd >= 0) close(fd);
        heap_caps_free(buf);
        httpd_resp_send_500(req);
        return;
    }

    char content_range[96] = "";
    if (partial) {
        snprintf(content_range, sizeof(content_range), "Content-Range: bytes %llu-%llu/%llu\r\n",
                 (unsigned long long)start, (unsigned long long)end, (unsigned long long)size);
    }
    char header[400];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\nContent-Type: video/x-msvideo\r\nContent-Length: %llu\r\n"
                     "Accept-Ranges: bytes\r\n%sContent-Disposition: attachment; filename=\"%s\"\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     partial ? "206 Partial Content" : "200 OK", (unsigned long long)len, content_range,
                     job->name);
    bool ok = send_all(req, header, n);

    uint64_t pos = start, remaining = len;
    if (ok && lseek(fd, (off_t)start, SEEK_SET) != (off_t)start) ok = false;
    while (ok && remaining > 0) {
        /* Keep reads aligned to the buffer size so the SD driver can use DMA. */
        size_t want = FILE_BUF_SIZE - (pos % FILE_BUF_SIZE);
        if (want > remaining) want = remaining;
        ssize_t r = read(fd, buf, want);
        if (r <= 0) break;
        ok = send_all(req, (const char *)buf, r);
        pos += r;
        remaining -= r;
    }
    close(fd);
    heap_caps_free(buf);
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
}

static void play_job(web_job_t *job)
{
    httpd_req_t *req = job->req;
    uint32_t us_per_frame = 0;
    uint16_t w = 0, h = 0;
    avi_reader_t *r = avi_reader_open(job->path, &us_per_frame, &w, &h);
    uint8_t *buf = heap_caps_malloc(PLAY_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!r || !buf) {
        avi_reader_close(r);
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "cannot open video");
        return;
    }
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const int64_t interval = (int64_t)us_per_frame / (job->speed > 0 ? job->speed : 1);
    int64_t next = esp_timer_get_time();
    int len;
    while ((len = avi_reader_next(r, buf, PLAY_BUF_SIZE)) > 0) {
        if (send_mjpeg_part(req, buf, len) != ESP_OK) break;
        next += interval;
        int64_t wait = next - esp_timer_get_time();
        if (wait > 1000) {
            vTaskDelay(pdMS_TO_TICKS(wait / 1000));
        } else if (wait < -1000000) {
            next = esp_timer_get_time();
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    avi_reader_close(r);
    heap_caps_free(buf);
}

/* ---------- handlers ---------- */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* custom.css comes from the SD card; keep it from pulling in anything off-device. */
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
                       "img-src 'self' data: blob:; font-src 'self' data:; frame-ancestors 'none'");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    web_job_t *job = calloc(1, sizeof(*job));
    if (!job) return httpd_resp_send_500(req);
    job->fn = stream_job;
    return start_async(req, job);
}

static esp_err_t snapshot_get(httpd_req_t *req)
{
    uint32_t seq = 0;
    frame_t *f = camera_acquire(&seq, 2000);
    if (!f) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "camera unavailable");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)f->data, f->len);
    camera_release(f);
    return err;
}

static esp_err_t media_get(httpd_req_t *req, const char *prefix, job_fn_t fn)
{
    char dir[8], name[64];
    web_job_t *job = calloc(1, sizeof(*job));
    if (!job) return httpd_resp_send_500(req);
    if (!parse_media_uri(req->uri, prefix, dir, sizeof(dir), name, sizeof(name)) ||
        !resolve_media(dir, name, job->path, sizeof(job->path))) {
        free(job);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (!storage_is_mounted()) {
        free(job);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "SD card not mounted");
    }
    strlcpy(job->name, name, sizeof(job->name));
    job->fn = fn;
    job->speed = 1;
    char query[32], value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "speed", value, sizeof(value)) == ESP_OK) {
        int speed = atoi(value);
        if (speed >= 1 && speed <= 8) job->speed = speed;
    }
    return start_async(req, job);
}

static esp_err_t files_download_get(httpd_req_t *req)
{
    return media_get(req, "/files/", download_job);
}

static esp_err_t play_get(httpd_req_t *req)
{
    return media_get(req, "/play/", play_job);
}

static void add_settings(cJSON *o, const dashcam_settings_t *s)
{
    cJSON_AddStringToObject(o, "sta_ssid", s->sta_ssid);
    cJSON_AddBoolToObject(o, "sta_pass_set", s->sta_pass[0] != '\0');
    cJSON_AddStringToObject(o, "ap_ssid", s->ap_ssid);
    cJSON_AddBoolToObject(o, "ap_pass_set", s->ap_pass[0] != '\0');
    cJSON_AddStringToObject(o, "hostname", s->hostname);
    cJSON_AddStringToObject(o, "tz", s->tz);
    cJSON_AddNumberToObject(o, "framesize", s->framesize);
    cJSON_AddNumberToObject(o, "jpeg_quality", s->jpeg_quality);
    cJSON_AddNumberToObject(o, "fps_limit", s->fps_limit);
    cJSON_AddNumberToObject(o, "segment_sec", s->segment_sec);
    cJSON_AddNumberToObject(o, "min_free_pct", s->min_free_pct);
    cJSON_AddBoolToObject(o, "vflip", s->vflip);
    cJSON_AddBoolToObject(o, "hmirror", s->hmirror);
    cJSON_AddBoolToObject(o, "auto_record", s->auto_record);
}

static esp_err_t status_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", app_version());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    time_t now = time(NULL);
    cJSON_AddNumberToObject(root, "time", (double)now);
    cJSON_AddBoolToObject(root, "time_valid", now > 1700000000);

    cJSON *cam = cJSON_AddObjectToObject(root, "camera");
    cJSON_AddBoolToObject(cam, "ok", camera_ok());
    cJSON_AddStringToObject(cam, "sensor", camera_sensor_name());
    cJSON_AddNumberToObject(cam, "fps", roundf(camera_fps() * 10) / 10);

    recorder_status_t rs;
    recorder_get_status(&rs);
    cJSON *rec = cJSON_AddObjectToObject(root, "rec");
    cJSON_AddBoolToObject(rec, "enabled", rs.enabled);
    cJSON_AddBoolToObject(rec, "recording", rs.recording);
    cJSON_AddStringToObject(rec, "file", rs.current_file);
    cJSON_AddNumberToObject(rec, "frames", rs.current_frames);
    cJSON_AddNumberToObject(rec, "bytes", (double)rs.current_bytes);
    cJSON_AddNumberToObject(rec, "fps", roundf(rs.write_fps * 10) / 10);
    cJSON_AddNumberToObject(rec, "deleted", rs.files_deleted);
    cJSON_AddStringToObject(rec, "error", rs.last_error);

    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(sd, "mounted", storage_is_mounted());
    cJSON_AddStringToObject(sd, "status", storage_status_text());
    uint64_t total = 0, free_bytes = 0;
    if (storage_space(&total, &free_bytes) == ESP_OK) {
        cJSON_AddNumberToObject(sd, "total", (double)total);
        cJSON_AddNumberToObject(sd, "free", (double)free_bytes);
    }

    net_status_t ns;
    net_get_status(&ns);
    cJSON *net = cJSON_AddObjectToObject(root, "net");
    cJSON_AddStringToObject(net, "hostname", ns.hostname);
    cJSON_AddBoolToObject(net, "sta_configured", ns.sta_configured);
    cJSON_AddBoolToObject(net, "sta_connected", ns.sta_connected);
    cJSON_AddStringToObject(net, "sta_ssid", ns.sta_ssid);
    cJSON_AddStringToObject(net, "sta_ip", ns.sta_ip);
    cJSON_AddNumberToObject(net, "sta_rssi", ns.sta_rssi);
    cJSON_AddStringToObject(net, "ap_ssid", ns.ap_ssid);
    cJSON_AddStringToObject(net, "ap_ip", ns.ap_ip);
    cJSON_AddBoolToObject(net, "ap_open", ns.ap_open);
    cJSON_AddNumberToObject(net, "ap_clients", ns.ap_clients);
    cJSON_AddBoolToObject(net, "time_synced", ns.time_synced);

    cJSON *usb = cJSON_AddObjectToObject(root, "usb");
    cJSON_AddBoolToObject(usb, "uvc", usb_uvc_running());
    cJSON_AddBoolToObject(usb, "streaming", usb_uvc_streaming());
    cJSON_AddBoolToObject(usb, "maintenance", app_is_maintenance());

    cJSON *mem = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(mem, "internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return send_json(req, root);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    dashcam_settings_t s;
    settings_get(&s);
    cJSON *root = cJSON_CreateObject();
    add_settings(root, &s);
    cJSON *sizes = cJSON_AddArrayToObject(root, "framesizes");
    for (size_t i = 0; i < sizeof(FRAME_SIZES) / sizeof(FRAME_SIZES[0]); i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "value", FRAME_SIZES[i].size);
        cJSON_AddStringToObject(item, "label", FRAME_SIZES[i].label);
        cJSON_AddItemToArray(sizes, item);
    }
    return send_json(req, root);
}

static void json_str(cJSON *j, const char *key, char *dst, size_t cap)
{
    const cJSON *item = cJSON_GetObjectItem(j, key);
    if (cJSON_IsString(item)) strlcpy(dst, item->valuestring, cap);
}

static void json_u8(cJSON *j, const char *key, uint8_t *dst)
{
    const cJSON *item = cJSON_GetObjectItem(j, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 255) *dst = item->valuedouble;
}

static void json_bool(cJSON *j, const char *key, bool *dst)
{
    const cJSON *item = cJSON_GetObjectItem(j, key);
    if (cJSON_IsBool(item)) *dst = cJSON_IsTrue(item);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_result(req, false, "JSONを読めません");

    dashcam_settings_t s, old;
    settings_get(&s);
    old = s;
    json_str(j, "sta_ssid", s.sta_ssid, sizeof(s.sta_ssid));
    json_str(j, "sta_pass", s.sta_pass, sizeof(s.sta_pass));
    json_str(j, "ap_ssid", s.ap_ssid, sizeof(s.ap_ssid));
    json_str(j, "hostname", s.hostname, sizeof(s.hostname));
    json_str(j, "tz", s.tz, sizeof(s.tz));
    const cJSON *ap_pass = cJSON_GetObjectItem(j, "ap_pass");
    if (cJSON_IsString(ap_pass)) {
        size_t len = strlen(ap_pass->valuestring);
        if (len > 0 && len < 8) {
            cJSON_Delete(j);
            return send_result(req, false, "APパスワードは8文字以上にしてください");
        }
        strlcpy(s.ap_pass, ap_pass->valuestring, sizeof(s.ap_pass));
    }
    json_u8(j, "framesize", &s.framesize);
    json_u8(j, "jpeg_quality", &s.jpeg_quality);
    json_u8(j, "fps_limit", &s.fps_limit);
    json_u8(j, "min_free_pct", &s.min_free_pct);
    const cJSON *seg = cJSON_GetObjectItem(j, "segment_sec");
    if (cJSON_IsNumber(seg) && seg->valuedouble > 0 && seg->valuedouble < 65535) s.segment_sec = seg->valuedouble;
    json_bool(j, "vflip", &s.vflip);
    json_bool(j, "hmirror", &s.hmirror);
    json_bool(j, "auto_record", &s.auto_record);
    cJSON_Delete(j);

    settings_sanitize(&s);
    const bool network_changed = strcmp(s.sta_ssid, old.sta_ssid) || strcmp(s.sta_pass, old.sta_pass) ||
                                 strcmp(s.ap_ssid, old.ap_ssid) || strcmp(s.ap_pass, old.ap_pass) ||
                                 strcmp(s.hostname, old.hostname);
    esp_err_t err = settings_save(&s);
    if (err != ESP_OK) return send_result(req, false, esp_err_to_name(err));

    camera_apply_settings(&s);
    setenv("TZ", s.tz, 1);
    tzset();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "reboot", network_changed);
    esp_err_t sent = send_json(req, root);
    if (network_changed) app_request_reboot(false);
    return sent;
}

static esp_err_t record_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    const cJSON *enabled = j ? cJSON_GetObjectItem(j, "enabled") : NULL;
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(j);
        return send_result(req, false, "enabled がありません");
    }
    recorder_set_enabled(cJSON_IsTrue(enabled));
    cJSON_Delete(j);
    return send_result(req, true, NULL);
}

static esp_err_t protect_post(httpd_req_t *req)
{
    recorder_protect_current();
    return send_result(req, true, "現在のファイルを保護して新しいファイルに切り替えます");
}

static esp_err_t time_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    const cJSON *ms = j ? cJSON_GetObjectItem(j, "epoch_ms") : NULL;
    if (!cJSON_IsNumber(ms) || ms->valuedouble < 1.7e12) {
        cJSON_Delete(j);
        return send_result(req, false, "epoch_ms が不正です");
    }
    const double epoch = ms->valuedouble / 1000.0;
    cJSON_Delete(j);
    net_status_t ns;
    net_get_status(&ns);
    if (!ns.time_synced || fabs((double)time(NULL) - epoch) > 30) {
        struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = (suseconds_t)(fmod(epoch, 1.0) * 1e6)};
        settimeofday(&tv, NULL);
        net_mark_time_set();
        ESP_LOGI(TAG, "clock set from browser");
    }
    return send_result(req, true, NULL);
}

static esp_err_t files_list_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!storage_is_mounted()) return httpd_resp_sendstr(req, "{\"mounted\":false,\"files\":[]}");

    char *buf = malloc(LIST_BUF_SIZE);
    if (!buf) return httpd_resp_send_500(req);
    size_t used = snprintf(buf, LIST_BUF_SIZE, "{\"mounted\":true,\"files\":[");
    bool first = true;
    const char *dirs[] = {"LOCK", "REC"};
    for (size_t i = 0; i < 2; i++) {
        char path[16];
        snprintf(path, sizeof(path), "%s/%s", storage_ff_drive(), dirs[i]);
        FF_DIR dir;
        FILINFO info;
        if (f_opendir(&dir, path) != FR_OK) continue;
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            if ((info.fattrib & AM_DIR) || !safe_name(info.fname)) continue;
            if (used + 160 > LIST_BUF_SIZE) {
                httpd_resp_send_chunk(req, buf, used);
                used = 0;
            }
            used += snprintf(buf + used, LIST_BUF_SIZE - used, "%s{\"d\":\"%s\",\"n\":\"%s\",\"s\":%llu}",
                             first ? "" : ",", dirs[i], info.fname, (unsigned long long)info.fsize);
            first = false;
        }
        f_closedir(&dir);
    }
    used += snprintf(buf + used, LIST_BUF_SIZE - used, "]}");
    httpd_resp_send_chunk(req, buf, used);
    free(buf);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t file_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_result(req, false, "JSONを読めません");
    char action[12] = "", dir[8] = "", name[64] = "", path[112];
    json_str(j, "action", action, sizeof(action));
    json_str(j, "dir", dir, sizeof(dir));
    json_str(j, "name", name, sizeof(name));
    cJSON_Delete(j);
    if (!resolve_media(dir, name, path, sizeof(path))) return send_result(req, false, "ファイル名が不正です");

    recorder_status_t rs;
    recorder_get_status(&rs);
    if (rs.recording && strcmp(rs.current_file, name) == 0) {
        return send_result(req, false, "録画中のファイルは操作できません");
    }

    int rc = -1;
    if (strcmp(action, "delete") == 0) {
        rc = unlink(path);
    } else if (strcmp(action, "lock") == 0 || strcmp(action, "unlock") == 0) {
        char to[112];
        snprintf(to, sizeof(to), SD_MOUNT_POINT "/%s/%s", strcmp(action, "lock") == 0 ? "LOCK" : "REC", name);
        rc = rename(path, to);
    } else {
        return send_result(req, false, "不明な操作です");
    }
    return send_result(req, rc == 0, rc == 0 ? NULL : "失敗しました");
}

/* ---------- UI assets on the SD card (www/) ---------- */

static const char *asset_type(const char *rel)
{
    const char *dot = strrchr(rel, '.');
    if (!dot) return NULL;
    for (size_t i = 0; i < sizeof(ASSET_TYPES) / sizeof(ASSET_TYPES[0]); i++) {
        if (strcasecmp(dot + 1, ASSET_TYPES[i].ext) == 0) return ASSET_TYPES[i].type;
    }
    return NULL;
}

/* "img/bg.png" -> "/sdcard/www/img/bg.png". Segments are [A-Za-z0-9_.-] and may not start
 * with '.', which rules out "..", hidden files and absolute paths. */
static bool resolve_asset(const char *rel, char *path, size_t cap, bool require_type)
{
    const size_t len = strlen(rel);
    if (len == 0 || len > ASSET_MAX_PATH || (require_type && !asset_type(rel))) return false;
    int depth = 0;
    const char *seg = rel;
    for (const char *p = rel;; p++) {
        if (*p == '/' || *p == '\0') {
            if (p == seg || *seg == '.' || ++depth > ASSET_MAX_DEPTH) return false;
            if (*p == '\0') break;
            seg = p + 1;
        } else if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') {
            return false;
        }
    }
    snprintf(path, cap, ASSET_DIR "/%s", rel);
    return true;
}

static esp_err_t serve_asset(httpd_req_t *req, const char *rel)
{
    char path[128];
    struct stat st;
    if (!resolve_asset(rel, path, sizeof(path), true) || !storage_is_mounted() || stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    char etag[48], match[48];
    snprintf(etag, sizeof(etag), "\"%llx-%llx\"", (unsigned long long)st.st_mtime,
             (unsigned long long)st.st_size);
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", match, sizeof(match)) == ESP_OK &&
        strcmp(match, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    int fd = open(path, O_RDONLY);
    char *buf = heap_caps_malloc(ASSET_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (fd < 0 || !buf) {
        if (fd >= 0) close(fd);
        heap_caps_free(buf);
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, asset_type(rel));
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Content-Security-Policy", ASSET_CSP);
    esp_err_t err = ESP_OK;
    ssize_t r;
    while (err == ESP_OK && (r = read(fd, buf, ASSET_BUF_SIZE)) > 0) {
        err = httpd_resp_send_chunk(req, buf, r);
    }
    close(fd);
    heap_caps_free(buf);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t assets_get(httpd_req_t *req)
{
    char rel[ASSET_MAX_PATH + 2];
    const char *p = req->uri + strlen("/assets/");
    const size_t len = strcspn(p, "?#");
    if (len >= sizeof(rel)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    memcpy(rel, p, len);
    rel[len] = '\0';
    return serve_asset(req, rel);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    return serve_asset(req, "favicon.ico");
}

static void list_assets(cJSON *files, const char *ff_path, const char *rel, int depth)
{
    FF_DIR dir;
    FILINFO info;
    if (f_opendir(&dir, ff_path) != FR_OK) return;
    while (cJSON_GetArraySize(files) < ASSET_MAX_LIST && f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        char child[ASSET_MAX_PATH + 2];
        if (snprintf(child, sizeof(child), "%s%s%s", rel, rel[0] ? "/" : "", info.fname) >= (int)sizeof(child)) {
            continue;
        }
        if (info.fattrib & AM_DIR) {
            char child_ff[ASSET_MAX_PATH + 16];
            if (depth + 1 < ASSET_MAX_DEPTH &&
                snprintf(child_ff, sizeof(child_ff), "%s/%s", ff_path, info.fname) < (int)sizeof(child_ff)) {
                list_assets(files, child_ff, child, depth + 1);
            }
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "p", child);
        cJSON_AddNumberToObject(item, "s", (double)info.fsize);
        cJSON_AddBoolToObject(item, "served", asset_type(child) != NULL);
        cJSON_AddItemToArray(files, item);
    }
    f_closedir(&dir);
}

static esp_err_t assets_list_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    const bool mounted = storage_is_mounted();
    cJSON_AddBoolToObject(root, "mounted", mounted);
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    if (mounted) {
        char ff_path[16];
        snprintf(ff_path, sizeof(ff_path), "%s/www", storage_ff_drive());
        list_assets(files, ff_path, "", 0);
    }
    return send_json(req, root);
}

static void make_parent_dirs(const char *path)
{
    char dir[128];
    strlcpy(dir, path, sizeof(dir));
    for (char *p = dir + strlen(SD_MOUNT_POINT) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(dir, 0775);
        *p = '/';
    }
}

/* Raw request body, target path in the X-Asset-Path header. */
static esp_err_t asset_upload_post(httpd_req_t *req)
{
    char rel[ASSET_MAX_PATH + 2] = "", path[128], tmp[136];
    if (httpd_req_get_hdr_value_str(req, "X-Asset-Path", rel, sizeof(rel)) != ESP_OK ||
        !resolve_asset(rel, path, sizeof(path), true)) {
        return send_result(req, false, "ファイル名が不正か、対応していない形式です");
    }
    if (!storage_is_mounted()) return send_result(req, false, "SDカードがありません");
    if (req->content_len <= 0 || req->content_len > ASSET_MAX_SIZE) {
        return send_result(req, false, "ファイルは 2MB までです");
    }

    make_parent_dirs(path);
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    char *buf = heap_caps_malloc(ASSET_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    bool ok = fd >= 0 && buf;
    size_t remaining = req->content_len, used = 0;
    int timeouts = 0;
    while (ok && remaining > 0) {
        size_t want = ASSET_BUF_SIZE - used;
        if (want > remaining) want = remaining;
        int r = httpd_req_recv(req, buf + used, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 10) continue;
        if (r <= 0) {
            ok = false;
            break;
        }
        used += r;
        remaining -= r;
        if (used == ASSET_BUF_SIZE || remaining == 0) {
            ok = write(fd, buf, used) == (ssize_t)used;
            used = 0;
        }
    }
    if (fd >= 0 && close(fd) != 0) ok = false;
    heap_caps_free(buf);
    if (ok) {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        unlink(tmp);
        return send_result(req, false, "SDカードに書き込めませんでした");
    }
    ESP_LOGI(TAG, "asset uploaded: %s (%u bytes)", rel, (unsigned)req->content_len);
    return send_result(req, true, NULL);
}

static esp_err_t asset_delete_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    char rel[ASSET_MAX_PATH + 2] = "", path[128];
    if (j) json_str(j, "path", rel, sizeof(rel));
    cJSON_Delete(j);
    if (!resolve_asset(rel, path, sizeof(path), false)) return send_result(req, false, "ファイル名が不正です");
    if (unlink(path) != 0) return send_result(req, false, "削除できませんでした");
    /* Drop directories left empty; rmdir refuses non-empty ones. */
    for (char *slash = strrchr(path, '/'); slash && slash > path + strlen(ASSET_DIR); slash = strrchr(path, '/')) {
        *slash = '\0';
        if (rmdir(path) != 0) break;
    }
    return send_result(req, true, NULL);
}

static esp_err_t format_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    char confirm[16] = "";
    if (j) json_str(j, "confirm", confirm, sizeof(confirm));
    cJSON_Delete(j);
    if (strcmp(confirm, "FORMAT") != 0) return send_result(req, false, "確認文字列が違います");
    recorder_stop_and_wait();
    esp_err_t err = storage_format();
    recorder_resume();
    return send_result(req, err == ESP_OK, err == ESP_OK ? "フォーマットしました" : esp_err_to_name(err));
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    char mode[16] = "normal";
    if (j) json_str(j, "mode", mode, sizeof(mode));
    cJSON_Delete(j);
    const bool maintenance = strcmp(mode, "maintenance") == 0;
    esp_err_t err = send_result(req, true, maintenance ? "メンテナンスモードで再起動します" : "再起動します");
    app_request_reboot(maintenance);
    return err;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len <= 0 || req->content_len > part->size) {
        return send_result(req, false, "ファームウェアのサイズが不正です");
    }
    ESP_LOGW(TAG, "OTA: receiving %u bytes into %s", (unsigned)req->content_len, part->label);
    recorder_stop_and_wait();

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    char *buf = malloc(4096);
    size_t remaining = req->content_len;
    int timeouts = 0;
    while (err == ESP_OK && buf && remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 10) continue;
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, r);
        remaining -= r;
    }
    free(buf);
    if (!buf && err == ESP_OK) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = esp_ota_end(ota);
    } else {
        esp_ota_abort(ota);
    }
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        recorder_resume();
        return send_result(req, false, esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "OTA complete, rebooting");
    esp_err_t sent = send_result(req, true, "更新しました。再起動します");
    app_request_reboot(false);
    return sent;
}

static esp_err_t log_get(httpd_req_t *req)
{
    const size_t cap = 24 * 1024;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    size_t len = logbuf_read(buf, cap);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, len);
    heap_caps_free(buf);
    return err;
}

/* Phones probe these on a new Wi-Fi network; redirecting opens the UI. */
static esp_err_t captive_redirect(httpd_req_t *req)
{
    net_status_t ns;
    net_get_status(&ns);
    char location[40];
    snprintf(location, sizeof(location), "http://%s/", ns.ap_ip[0] ? ns.ap_ip : "192.168.4.1");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    return captive_redirect(req);
}

esp_err_t web_start(void)
{
    s_async_slots = xSemaphoreCreateCounting(MAX_ASYNC_JOBS, MAX_ASYNC_JOBS);
    if (!s_async_slots) return ESP_ERR_NO_MEM;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.max_open_sockets = 10;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.send_wait_timeout = 10;
    config.recv_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/index.html", .method = HTTP_GET, .handler = index_get},
        {.uri = "/stream", .method = HTTP_GET, .handler = stream_get},
        {.uri = "/snapshot.jpg", .method = HTTP_GET, .handler = snapshot_get},
        {.uri = "/files/*", .method = HTTP_GET, .handler = files_download_get},
        {.uri = "/play/*", .method = HTTP_GET, .handler = play_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
        {.uri = "/api/record", .method = HTTP_POST, .handler = record_post},
        {.uri = "/api/protect", .method = HTTP_POST, .handler = protect_post},
        {.uri = "/api/time", .method = HTTP_POST, .handler = time_post},
        {.uri = "/api/files", .method = HTTP_GET, .handler = files_list_get},
        {.uri = "/api/file", .method = HTTP_POST, .handler = file_post},
        {.uri = "/api/format", .method = HTTP_POST, .handler = format_post},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/assets/*", .method = HTTP_GET, .handler = assets_get},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get},
        {.uri = "/api/assets", .method = HTTP_GET, .handler = assets_list_get},
        {.uri = "/api/assets/upload", .method = HTTP_POST, .handler = asset_upload_post},
        {.uri = "/api/assets/delete", .method = HTTP_POST, .handler = asset_delete_post},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = captive_redirect},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
