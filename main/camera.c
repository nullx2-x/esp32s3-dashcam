#include "camera.h"

#include <string.h>

#include "board.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define HUB_SLOTS          8
#define SLOT_INITIAL_CAP   (192 * 1024)
#define EV_NEW_FRAME       BIT0

static const char *TAG = "camera";

static frame_t s_slots[HUB_SLOTS];
static frame_t *s_latest;
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;

static dashcam_settings_t s_pending;
static volatile bool s_pending_valid;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;

static bool s_ok;
static float s_fps;
static const char *s_sensor_name = "none";

static void apply_sensor_settings(const dashcam_settings_t *st)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    s->set_framesize(s, (framesize_t)st->framesize);
    s->set_quality(s, st->jpeg_quality);
    s->set_vflip(s, st->vflip);
    s->set_hmirror(s, st->hmirror);
    ESP_LOGI(TAG, "framesize=%d quality=%d vflip=%d hmirror=%d",
             st->framesize, st->jpeg_quality, st->vflip, st->hmirror);
}

static frame_t *claim_slot(size_t need)
{
    frame_t *found = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < HUB_SLOTS; i++) {
        frame_t *f = &s_slots[i];
        if (f->refs == 0 && f != s_latest) {
            f->refs = 1;
            found = f;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (!found) return NULL;

    if (found->cap < need) {
        size_t cap = need > SLOT_INITIAL_CAP ? need + need / 4 : SLOT_INITIAL_CAP;
        uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            found->refs = 0;
            xSemaphoreGive(s_lock);
            return NULL;
        }
        heap_caps_free(found->data);
        found->data = buf;
        found->cap = cap;
    }
    return found;
}

static void publish(frame_t *f)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    f->seq = ++s_seq;
    f->refs = 0;
    s_latest = f;
    xSemaphoreGive(s_lock);
    /* Wakes every waiter; clearing right away keeps later waiters blocked. */
    xEventGroupSetBits(s_events, EV_NEW_FRAME);
    xEventGroupClearBits(s_events, EV_NEW_FRAME);
}

static void camera_task(void *arg)
{
    int64_t fps_window_start = esp_timer_get_time();
    int fps_frames = 0;
    int failures = 0;

    for (;;) {
        if (s_pending_valid) {
            dashcam_settings_t st;
            portENTER_CRITICAL(&s_pending_lock);
            st = s_pending;
            s_pending_valid = false;
            portEXIT_CRITICAL(&s_pending_lock);
            apply_sensor_settings(&st);
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            if (++failures % 20 == 1) ESP_LOGW(TAG, "frame capture failed (%d)", failures);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        failures = 0;

        frame_t *slot = claim_slot(fb->len);
        if (slot) {
            memcpy(slot->data, fb->buf, fb->len);
            slot->len = fb->len;
            slot->width = fb->width;
            slot->height = fb->height;
            slot->timestamp_us = esp_timer_get_time();
            publish(slot);
            fps_frames++;
        }
        esp_camera_fb_return(fb);

        int64_t now = esp_timer_get_time();
        if (now - fps_window_start >= 2000000) {
            s_fps = fps_frames * 1e6f / (float)(now - fps_window_start);
            fps_frames = 0;
            fps_window_start = now;
        }
    }
}

esp_err_t camera_start(const dashcam_settings_t *settings)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_lock || !s_events) return ESP_ERR_NO_MEM;

    const camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        /* Buffers are sized for the largest resolution so the UI can switch freely. */
        .frame_size = FRAMESIZE_QXGA,
        .jpeg_quality = settings->jpeg_quality,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
        if (info) s_sensor_name = info->name;
        ESP_LOGI(TAG, "sensor %s (PID 0x%04x)", s_sensor_name, s->id.PID);
    }
    apply_sensor_settings(settings);
    s_ok = true;

    if (xTaskCreatePinnedToCore(camera_task, "camera", 4096, NULL, 6, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void camera_apply_settings(const dashcam_settings_t *settings)
{
    portENTER_CRITICAL(&s_pending_lock);
    s_pending = *settings;
    s_pending_valid = true;
    portEXIT_CRITICAL(&s_pending_lock);
}

frame_t *camera_acquire(uint32_t *last_seq, uint32_t timeout_ms)
{
    if (!s_ok) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return NULL;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        frame_t *f = s_latest;
        if (f && f->seq != *last_seq) {
            f->refs++;
            *last_seq = f->seq;
            xSemaphoreGive(s_lock);
            return f;
        }
        xSemaphoreGive(s_lock);

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return NULL;
        TickType_t wait = deadline - now;
        if (wait > pdMS_TO_TICKS(100)) wait = pdMS_TO_TICKS(100);
        xEventGroupWaitBits(s_events, EV_NEW_FRAME, pdFALSE, pdFALSE, wait);
    }
}

void camera_release(frame_t *frame)
{
    if (!frame) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (frame->refs > 0) frame->refs--;
    xSemaphoreGive(s_lock);
}

bool camera_ok(void)
{
    return s_ok;
}

float camera_fps(void)
{
    return s_fps;
}

const char *camera_sensor_name(void)
{
    return s_sensor_name;
}
