#include "recorder.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "avi.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"
#include "storage.h"

#define MIN_FREE_FLOOR_BYTES (256ULL * 1024 * 1024)
#define SYNC_INTERVAL_US     (3 * 1000000LL)
#define REMOUNT_INTERVAL_US  (10 * 1000000LL)

static const char *TAG = "recorder";

static volatile bool s_enabled;
static volatile bool s_paused;
static volatile bool s_idle;
static volatile bool s_protect_request;
static uint32_t s_next_seq = 1;

static SemaphoreHandle_t s_status_lock;
static recorder_status_t s_status;

static void set_error(const char *fmt, const char *detail)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    snprintf(s_status.last_error, sizeof(s_status.last_error), fmt, detail ? detail : "");
    xSemaphoreGive(s_status_lock);
}

static uint32_t parse_seq(const char *name)
{
    return (uint32_t)strtoul(name, NULL, 10);
}

static void scan_next_seq(void)
{
    uint32_t max_seq = 0;
    const char *dirs[] = {REC_DIR, LOCK_DIR};
    for (size_t i = 0; i < 2; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            uint32_t seq = parse_seq(e->d_name);
            if (seq > max_seq) max_seq = seq;
        }
        closedir(d);
    }
    s_next_seq = max_seq + 1;
    ESP_LOGI(TAG, "next segment number %lu", (unsigned long)s_next_seq);
}

static bool delete_oldest(void)
{
    DIR *d = opendir(REC_DIR);
    if (!d) return false;
    char oldest[64] = "";
    uint32_t oldest_seq = UINT32_MAX;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;
        uint32_t seq = parse_seq(e->d_name);
        if (seq > 0 && seq < oldest_seq && strlen(e->d_name) < sizeof(oldest)) {
            oldest_seq = seq;
            strlcpy(oldest, e->d_name, sizeof(oldest));
        }
    }
    closedir(d);
    if (!oldest[0]) return false;

    char path[96];
    snprintf(path, sizeof(path), REC_DIR "/%s", oldest);
    if (unlink(path) != 0) return false;
    ESP_LOGI(TAG, "loop recording: deleted %s", oldest);
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.files_deleted++;
    xSemaphoreGive(s_status_lock);
    return true;
}

static esp_err_t ensure_space(uint8_t min_free_pct)
{
    for (int guard = 0; guard < 1000; guard++) {
        uint64_t total = 0, free_bytes = 0;
        if (storage_space(&total, &free_bytes) != ESP_OK) return ESP_FAIL;
        uint64_t want = total * min_free_pct / 100;
        if (want < MIN_FREE_FLOOR_BYTES) want = MIN_FREE_FLOOR_BYTES;
        if (free_bytes >= want) return ESP_OK;
        if (!delete_oldest()) return ESP_ERR_NO_MEM;
    }
    return ESP_ERR_NO_MEM;
}

static void segment_name(char *out, size_t cap, uint32_t seq)
{
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(out, cap, "%06lu_%04d%02d%02d_%02d%02d%02d.avi", (unsigned long)seq, tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(out, cap, "%06lu_notime.avi", (unsigned long)seq);
    }
}

static void finish_segment(avi_writer_t **w, const char *name, bool protect)
{
    if (!*w) return;
    uint32_t frames = avi_writer_frames(*w);
    esp_err_t err = avi_writer_close(*w);
    *w = NULL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "closing %s failed", name);
        set_error("ファイルを閉じられませんでした: %s", name);
    } else {
        ESP_LOGI(TAG, "segment %s done (%lu frames)", name, (unsigned long)frames);
    }
    if (protect) {
        char from[96], to[96];
        snprintf(from, sizeof(from), REC_DIR "/%s", name);
        snprintf(to, sizeof(to), LOCK_DIR "/%s", name);
        if (rename(from, to) == 0) {
            ESP_LOGI(TAG, "protected %s", name);
        } else {
            ESP_LOGE(TAG, "protecting %s failed", name);
        }
    }
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.recording = false;
    s_status.current_file[0] = '\0';
    s_status.current_frames = 0;
    s_status.current_bytes = 0;
    xSemaphoreGive(s_status_lock);
}

static void recorder_task(void *arg)
{
    avi_writer_t *w = NULL;
    char name[64] = "";
    uint16_t seg_w = 0, seg_h = 0;
    int64_t seg_start = 0, last_sync = 0, next_due = 0, last_mount_try = -REMOUNT_INTERVAL_US;
    int64_t fps_window = esp_timer_get_time();
    uint32_t fps_frames = 0;
    uint32_t last_seq = 0;
    dashcam_settings_t st;
    settings_get(&st);
    int64_t settings_refreshed = 0;

    for (;;) {
        if (!s_enabled || s_paused) {
            finish_segment(&w, name, false);
            s_idle = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        s_idle = false;

        int64_t now = esp_timer_get_time();
        if (!storage_is_mounted()) {
            if (now - last_mount_try >= REMOUNT_INTERVAL_US) {
                last_mount_try = now;
                if (storage_mount() == ESP_OK) {
                    scan_next_seq();
                    set_error("%s", "");
                } else {
                    set_error("SDカード: %s", storage_status_text());
                }
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (now - settings_refreshed > 1000000) {
            settings_get(&st);
            settings_refreshed = now;
        }

        frame_t *f = camera_acquire(&last_seq, 1000);
        if (!f) continue;

        /* Pace against a schedule rather than the previous frame, otherwise a camera
         * running slightly faster than the limit gets every other frame dropped. */
        const int64_t min_interval = 1000000LL / st.fps_limit;
        if (w && f->timestamp_us < next_due) {
            camera_release(f);
            continue;
        }
        next_due = next_due + min_interval > f->timestamp_us ? next_due + min_interval
                                                               : f->timestamp_us + min_interval / 2;

        bool protect = s_protect_request;
        if (w && (protect || f->width != seg_w || f->height != seg_h ||
                  f->timestamp_us - seg_start >= (int64_t)st.segment_sec * 1000000LL)) {
            if (protect) s_protect_request = false;
            finish_segment(&w, name, protect);
        }

        if (!w) {
            esp_err_t err = ensure_space(st.min_free_pct);
            if (err != ESP_OK) {
                camera_release(f);
                set_error("%s", err == ESP_ERR_NO_MEM ? "SDカードの空きがありません（保護ファイルを整理してください）"
                                                      : "空き容量を取得できません");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            segment_name(name, sizeof(name), s_next_seq++);
            char path[96];
            snprintf(path, sizeof(path), REC_DIR "/%s", name);
            w = avi_writer_open(path, f->width, f->height, st.fps_limit);
            if (!w) {
                camera_release(f);
                set_error("録画ファイルを作成できません: %s", name);
                storage_unmount();
                continue;
            }
            seg_w = f->width;
            seg_h = f->height;
            seg_start = f->timestamp_us;
            last_sync = now;
            ESP_LOGI(TAG, "recording %s (%ux%u)", name, seg_w, seg_h);
            xSemaphoreTake(s_status_lock, portMAX_DELAY);
            s_status.recording = true;
            strlcpy(s_status.current_file, name, sizeof(s_status.current_file));
            s_status.last_error[0] = '\0';
            xSemaphoreGive(s_status_lock);
        }

        esp_err_t err = avi_writer_add_frame(w, f->data, f->len, f->timestamp_us);
        camera_release(f);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write failed, remounting card");
            set_error("書き込みエラー: %s", name);
            avi_writer_close(w);
            w = NULL;
            storage_unmount();
            continue;
        }
        fps_frames++;

        now = esp_timer_get_time();
        if (now - last_sync >= SYNC_INTERVAL_US) {
            avi_writer_sync(w);
            last_sync = now;
        }
        if (now - fps_window >= 2000000) {
            xSemaphoreTake(s_status_lock, portMAX_DELAY);
            s_status.write_fps = fps_frames * 1e6f / (float)(now - fps_window);
            xSemaphoreGive(s_status_lock);
            fps_frames = 0;
            fps_window = now;
        }
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        s_status.current_frames = avi_writer_frames(w);
        s_status.current_bytes = avi_writer_bytes(w);
        xSemaphoreGive(s_status_lock);
    }
}

esp_err_t recorder_start(bool enabled)
{
    s_status_lock = xSemaphoreCreateMutex();
    if (!s_status_lock) return ESP_ERR_NO_MEM;
    s_enabled = enabled;
    if (storage_is_mounted()) scan_next_seq();
    if (xTaskCreatePinnedToCore(recorder_task, "recorder", 6144, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void recorder_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

void recorder_get_status(recorder_status_t *out)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_lock);
    out->enabled = s_enabled;
}

void recorder_stop_and_wait(void)
{
    s_paused = true;
    for (int i = 0; i < 100 && !s_idle; i++) vTaskDelay(pdMS_TO_TICKS(50));
}

void recorder_resume(void)
{
    s_paused = false;
}

void recorder_protect_current(void)
{
    s_protect_request = true;
}
