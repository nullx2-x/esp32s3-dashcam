#include <stdlib.h>
#include <time.h>

#include "app.h"
#include "board.h"
#include "camera.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "net.h"
#include "nvs_flash.h"
#include "recorder.h"
#include "settings.h"
#include "storage.h"
#include "usb_uvc.h"
#include "web.h"

#define MAINTENANCE_MAGIC 0x4D41494EU /* "MAIN" */
#define BOOT_HOLD_MS      3000

static const char *TAG = "dashcam";

static RTC_NOINIT_ATTR uint32_t s_boot_request;
static bool s_maintenance;

static void reboot_task(void *arg)
{
    const bool maintenance = (bool)(intptr_t)arg;
    ESP_LOGW(TAG, "rebooting%s", maintenance ? " into maintenance mode" : "");
    recorder_stop_and_wait();
    storage_unmount();
    vTaskDelay(pdMS_TO_TICKS(300));
    if (maintenance) {
        s_boot_request = MAINTENANCE_MAGIC;
        usb_restore_serial_jtag();
    }
    esp_restart();
}

void app_request_reboot(bool maintenance)
{
    static bool requested;
    if (requested) return;
    requested = true;
    xTaskCreate(reboot_task, "reboot", 4096, (void *)(intptr_t)maintenance, 10, NULL);
}

bool app_is_maintenance(void)
{
    return s_maintenance;
}

const char *app_version(void)
{
    return esp_app_get_description()->version;
}

static void boot_button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held_ms = gpio_get_level(BOOT_BUTTON_GPIO) == 0 ? held_ms + 100 : 0;
        if (held_ms >= BOOT_HOLD_MS) {
            ESP_LOGW(TAG, "BOOT button held: switching USB to flashing mode");
            app_request_reboot(!s_maintenance);
            held_ms = -100000;
        }
    }
}

void app_main(void)
{
    s_maintenance = s_boot_request == MAINTENANCE_MAGIC;
    s_boot_request = 0;
    if (s_maintenance) usb_restore_serial_jtag();

    logbuf_init();
    ESP_LOGI(TAG, "XIAO ESP32S3 dashcam %s%s", app_version(), s_maintenance ? " [maintenance mode]" : "");
    usb_log_phy_state();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    settings_init();
    dashcam_settings_t st;
    settings_get(&st);
    setenv("TZ", st.tz, 1);
    tzset();

    if (camera_start(&st) != ESP_OK) ESP_LOGE(TAG, "camera unavailable; web UI still starts");
    if (storage_mount() != ESP_OK) ESP_LOGW(TAG, "SD card: %s", storage_status_text());
    ESP_ERROR_CHECK(recorder_start(st.auto_record));

    ESP_ERROR_CHECK(net_start(&st));
    ESP_ERROR_CHECK(web_start());

    if (!s_maintenance && usb_uvc_start() == ESP_OK) {
        /* The USB-Serial/JTAG console is gone now; keep logs in RAM only. */
        logbuf_set_console(false);
    }

    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 2, NULL);

    /* Reaching this point means an OTA image is good enough to keep. */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "ready");
}
