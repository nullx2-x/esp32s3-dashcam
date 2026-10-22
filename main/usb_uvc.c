#include "usb_uvc.h"

#include <inttypes.h>

#include "camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "sdkconfig.h"

#if CONFIG_DASHCAM_ENABLE_UVC
#include "usb_device_uvc.h"
#endif

#define UVC_BUFFER_SIZE (512 * 1024)

static const char *TAG = "uvc";

static bool s_running;
static volatile bool s_streaming;

#if CONFIG_DASHCAM_ENABLE_UVC
static uvc_fb_t s_fb;
static frame_t *s_frame;
static uint32_t s_last_seq;

static esp_err_t on_start(uvc_format_t format, int width, int height, int rate, void *ctx)
{
    ESP_LOGI(TAG, "host opened stream %dx%d @%d fps", width, height, rate);
    s_streaming = true;
    return ESP_OK;
}

static uvc_fb_t *on_fb_get(void *ctx)
{
    frame_t *f = camera_acquire(&s_last_seq, 1000);
    if (!f) return NULL;
    s_frame = f;
    s_fb.buf = f->data;
    s_fb.len = f->len;
    s_fb.width = f->width;
    s_fb.height = f->height;
    s_fb.format = UVC_FORMAT_JPEG;
    s_fb.timestamp.tv_sec = f->timestamp_us / 1000000;
    s_fb.timestamp.tv_usec = f->timestamp_us % 1000000;
    return &s_fb;
}

static void on_fb_return(uvc_fb_t *fb, void *ctx)
{
    camera_release(s_frame);
    s_frame = NULL;
}

static void on_stop(void *ctx)
{
    if (s_streaming) ESP_LOGI(TAG, "host closed stream");
    s_streaming = false;
}
#endif

esp_err_t usb_uvc_start(void)
{
#if CONFIG_DASHCAM_ENABLE_UVC
    uint8_t *buffer = heap_caps_malloc(UVC_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!buffer) return ESP_ERR_NO_MEM;
    uvc_device_config_t config = {
        .uvc_buffer = buffer,
        .uvc_buffer_size = UVC_BUFFER_SIZE,
        .start_cb = on_start,
        .fb_get_cb = on_fb_get,
        .fb_return_cb = on_fb_return,
        .stop_cb = on_stop,
    };
    esp_err_t err = uvc_device_config(0, &config);
    if (err == ESP_OK) err = uvc_device_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UVC start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_running = true;
    ESP_LOGI(TAG, "USB webcam enabled (%dx%d MJPEG)", CONFIG_UVC_CAM1_FRAMESIZE_WIDTH,
             CONFIG_UVC_CAM1_FRAMESIZE_HEIGT);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool usb_uvc_running(void)
{
    return s_running;
}

bool usb_uvc_streaming(void)
{
    return s_streaming;
}

void usb_restore_serial_jtag(void)
{
    /* Hold D+ low long enough for the host to notice a disconnect; otherwise it
     * keeps talking to the old webcam device and never enumerates the serial port. */
    const usb_wrap_pull_override_vals_t wrap_off = {.dp_pd = true, .dm_pd = true};
    const usb_serial_jtag_pull_override_vals_t usj_off = {.dp_pd = true, .dm_pd = true};
    usb_wrap_ll_phy_enable_pull_override(&USB_WRAP, &wrap_off);
    usb_serial_jtag_ll_phy_enable_pull_override(&usj_off);

    /* The PHY routing lives in the RTC domain and survives a software reset. */
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
    esp_rom_delay_us(300 * 1000);

    /* The pull bits keep their values after the override is released. */
    const usb_wrap_pull_override_vals_t wrap_default = {0};
    const usb_serial_jtag_pull_override_vals_t usj_default = {.dp_pu = true};
    usb_wrap_ll_phy_enable_pull_override(&USB_WRAP, &wrap_default);
    usb_serial_jtag_ll_phy_enable_pull_override(&usj_default);
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_wrap_ll_phy_disable_pull_override(&USB_WRAP);
}

void usb_log_phy_state(void)
{
    ESP_LOGI(TAG, "USJ conf0=0x%08" PRIx32 " RTC usb_conf=0x%08" PRIx32 " WRAP otg_conf=0x%08" PRIx32,
             USB_SERIAL_JTAG.conf0.val, RTCCNTL.usb_conf.val, USB_WRAP.otg_conf.val);
}
