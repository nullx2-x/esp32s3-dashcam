#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"
#define REC_DIR        SD_MOUNT_POINT "/REC"
#define LOCK_DIR       SD_MOUNT_POINT "/LOCK"

esp_err_t storage_mount(void);
void storage_unmount(void);
bool storage_is_mounted(void);
esp_err_t storage_space(uint64_t *total_bytes, uint64_t *free_bytes);
esp_err_t storage_format(void);
const char *storage_status_text(void);
uint32_t storage_card_khz(void);
/* FATFS drive prefix (e.g. "0:") for native ff.h calls that also return file sizes. */
const char *storage_ff_drive(void);
