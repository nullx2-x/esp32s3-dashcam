#pragma once

/* Seeed Studio XIAO ESP32S3 Sense (camera board + microSD expansion). */

#define CAM_PIN_PWDN   (-1)
#define CAM_PIN_RESET  (-1)
#define CAM_PIN_XCLK   10
#define CAM_PIN_SIOD   40
#define CAM_PIN_SIOC   39
#define CAM_PIN_D7     48
#define CAM_PIN_D6     11
#define CAM_PIN_D5     12
#define CAM_PIN_D4     14
#define CAM_PIN_D3     16
#define CAM_PIN_D2     18
#define CAM_PIN_D1     17
#define CAM_PIN_D0     15
#define CAM_PIN_VSYNC  38
#define CAM_PIN_HREF   47
#define CAM_PIN_PCLK   13

/* microSD slot is wired for SPI mode. GPIO21 is shared with the user LED. */
#define SD_PIN_CS      21
#define SD_PIN_SCK     7
#define SD_PIN_MISO    8
#define SD_PIN_MOSI    9

#define BOOT_BUTTON_GPIO 0
