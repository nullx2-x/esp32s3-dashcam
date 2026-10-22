#include "storage.h"

#include <stdio.h>
#include <sys/stat.h>

#include "board.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

static sdmmc_card_t *s_card;
static bool s_bus_ready;
static bool s_spi_mode;
static volatile bool s_mounted;
static char s_status[64] = "未初期化";
static char s_drive[8] = "0:";
static SemaphoreHandle_t s_lock;

static void ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static esp_err_t ensure_bus(void)
{
    if (s_bus_ready) return ESP_OK;
    const spi_bus_config_t bus = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32 * 1024,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) s_bus_ready = true;
    return err;
}

static void release_bus(void)
{
    if (!s_bus_ready) return;
    spi_bus_free(SPI2_HOST);
    s_bus_ready = false;
}

/* The slot's SPI wiring also works as a 1-bit SD bus (SCK=CLK, MOSI=CMD,
 * MISO=D0, CS=D3), which avoids most of the SPI protocol overhead. */
static esp_err_t try_sdmmc(const esp_vfs_fat_sdmmc_mount_config_t *mount_config, int khz)
{
    gpio_reset_pin(SD_PIN_CS);
    gpio_set_direction(SD_PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(SD_PIN_CS, 1); /* D3 high keeps the card in SD mode */

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = khz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_PIN_SCK;
    slot.cmd = SD_PIN_MOSI;
    slot.d0 = SD_PIN_MISO;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, mount_config, &s_card);
}

static esp_err_t try_spi(const esp_vfs_fat_sdmmc_mount_config_t *mount_config, int khz)
{
    gpio_reset_pin(SD_PIN_CS);
    esp_err_t err = ensure_bus();
    if (err != ESP_OK) return err;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SPI2_HOST;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = khz;
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, mount_config, &s_card);
    if (err != ESP_OK) release_bus();
    return err;
}

static esp_err_t mount_locked(bool format_if_failed)
{
    if (s_mounted) return ESP_OK;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_failed,
        .max_files = 10,
        .allocation_unit_size = 32 * 1024,
    };

    static const struct {
        bool spi;
        int khz;
    } attempts[] = {
        {false, SDMMC_FREQ_HIGHSPEED},
        {false, SDMMC_FREQ_DEFAULT},
        {true, SDMMC_FREQ_HIGHSPEED},
        {true, SDMMC_FREQ_DEFAULT},
    };
    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        err = attempts[i].spi ? try_spi(&mount_config, attempts[i].khz)
                              : try_sdmmc(&mount_config, attempts[i].khz);
        if (err == ESP_OK) {
            s_spi_mode = attempts[i].spi;
            break;
        }
        ESP_LOGW(TAG, "%s mount at %d kHz failed: %s", attempts[i].spi ? "SPI" : "SDMMC", attempts[i].khz,
                 esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            snprintf(s_status, sizeof(s_status), "FAT32として読めません（要フォーマット）");
        } else {
            snprintf(s_status, sizeof(s_status), "SDカードなし／認識失敗 (%s)", esp_err_to_name(err));
        }
        return err;
    }

    mkdir(REC_DIR, 0775);
    mkdir(LOCK_DIR, 0775);
    snprintf(s_drive, sizeof(s_drive), "%u:", ff_diskio_get_pdrv_card(s_card));
    s_mounted = true;
    uint64_t total = 0, free_bytes = 0;
    esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes);
    snprintf(s_status, sizeof(s_status), "OK %.1fGB (%s, %s %luMHz)", total / 1e9, s_card->cid.name,
             s_spi_mode ? "SPI" : "SD 1bit", (unsigned long)s_card->real_freq_khz / 1000);
    ESP_LOGI(TAG, "mounted %s via %s: %.1f GB total, %.1f GB free, %lu kHz", s_card->cid.name,
             s_spi_mode ? "SPI" : "SDMMC 1-bit", total / 1e9, free_bytes / 1e9,
             (unsigned long)s_card->real_freq_khz);
    return ESP_OK;
}

static void unmount_locked(void)
{
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (s_spi_mode) release_bus();
    s_card = NULL;
    s_mounted = false;
    snprintf(s_status, sizeof(s_status), "取り外し済み");
}

esp_err_t storage_mount(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = mount_locked(false);
    xSemaphoreGive(s_lock);
    return err;
}

void storage_unmount(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unmount_locked();
    xSemaphoreGive(s_lock);
}

bool storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t storage_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    return esp_vfs_fat_info(SD_MOUNT_POINT, total_bytes, free_bytes);
}

esp_err_t storage_format(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err;
    if (s_mounted) {
        err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
        if (err == ESP_OK) {
            mkdir(REC_DIR, 0775);
            mkdir(LOCK_DIR, 0775);
        }
    } else {
        err = mount_locked(true);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "format result: %s", esp_err_to_name(err));
    return err;
}

const char *storage_status_text(void)
{
    return s_status;
}

uint32_t storage_card_khz(void)
{
    return (s_mounted && s_card) ? s_card->real_freq_khz : 0;
}

const char *storage_ff_drive(void)
{
    return s_drive;
}
