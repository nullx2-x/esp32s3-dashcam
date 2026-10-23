#include "camera.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define HUB_SLOTS          8
#define SLOT_INITIAL_CAP   (192 * 1024)
#define EV_NEW_FRAME       BIT0
#define STALL_TIMEOUT_US   (10 * 1000000LL)
#define RETRY_MIN_MS       5000
#define RETRY_MAX_MS       60000

static const char *TAG = "camera";

static frame_t s_slots[HUB_SLOTS];
static frame_t *s_latest;
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;

static dashcam_settings_t s_pending;
static volatile bool s_pending_valid;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;

static camera_config_t s_config;
static dashcam_settings_t s_settings;
static volatile bool s_ok;
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

/* The sensor has no power or reset line on this board, so it survives an ESP32 reset. If that
 * reset landed mid-transfer the sensor may still hold SDA low; clocking SCL releases it. */
static void sccb_bus_recover(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = BIT64(CAM_PIN_SIOD) | BIT64(CAM_PIN_SIOC),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    gpio_set_level(CAM_PIN_SIOD, 1);
    gpio_set_level(CAM_PIN_SIOC, 1);
    esp_rom_delay_us(20);
    const int sda = gpio_get_level(CAM_PIN_SIOD), scl = gpio_get_level(CAM_PIN_SIOC);
    if (!sda || !scl) ESP_LOGW(TAG, "SCCB bus not idle (SDA=%d SCL=%d), clocking it free", sda, scl);
    for (int i = 0; i < 9 && !gpio_get_level(CAM_PIN_SIOD); i++) {
        gpio_set_level(CAM_PIN_SIOC, 0);
        esp_rom_delay_us(10);
        gpio_set_level(CAM_PIN_SIOC, 1);
        esp_rom_delay_us(10);
    }
    /* STOP condition */
    gpio_set_level(CAM_PIN_SIOD, 0);
    esp_rom_delay_us(10);
    gpio_set_level(CAM_PIN_SIOD, 1);
    esp_rom_delay_us(10);
    gpio_reset_pin(CAM_PIN_SIOD);
    gpio_reset_pin(CAM_PIN_SIOC);
}

static esp_err_t init_sensor(void)
{
    sccb_bus_recover();
    esp_err_t err = esp_camera_init(&s_config);
    if (err != ESP_OK) {
        esp_camera_deinit();
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
        if (info) s_sensor_name = info->name;
        ESP_LOGI(TAG, "sensor %s (PID 0x%04x)", s_sensor_name, s->id.PID);
    }
    apply_sensor_settings(&s_settings);
    return ESP_OK;
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
    int64_t last_frame = fps_window_start;
    int fps_frames = 0;
    uint32_t retry_ms = RETRY_MIN_MS;

    for (;;) {
        if (s_pending_valid) {
            portENTER_CRITICAL(&s_pending_lock);
            s_settings = s_pending;
            s_pending_valid = false;
            portEXIT_CRITICAL(&s_pending_lock);
            if (s_ok) apply_sensor_settings(&s_settings);
        }

        if (!s_ok) {
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            esp_err_t err = init_sensor();
            if (err != ESP_OK) {
                retry_ms = retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry_ms * 2;
                ESP_LOGE(TAG, "camera still unavailable (%s), next try in %lu s", esp_err_to_name(err),
                         (unsigned long)retry_ms / 1000);
                continue;
            }
            ESP_LOGW(TAG, "camera recovered");
            retry_ms = RETRY_MIN_MS;
            last_frame = fps_window_start = esp_timer_get_time();
            fps_frames = 0;
            s_ok = true;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            if (esp_timer_get_time() - last_frame > STALL_TIMEOUT_US) {
                ESP_LOGE(TAG, "no frames for %lld s, restarting camera", STALL_TIMEOUT_US / 1000000);
                s_ok = false;
                s_fps = 0;
                esp_camera_deinit();
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }
        last_frame = esp_timer_get_time();

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

    s_config = (camera_config_t){
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

    s_settings = *settings;

    esp_err_t err = init_sensor();
    if (err != ESP_OK) ESP_LOGE(TAG, "camera init failed: %s; retrying in the background", esp_err_to_name(err));
    s_ok = err == ESP_OK;

    if (xTaskCreatePinnedToCore(camera_task, "camera", 4096, NULL, 6, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return err;
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
