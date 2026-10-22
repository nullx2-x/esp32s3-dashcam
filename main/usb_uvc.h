#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t usb_uvc_start(void);
bool usb_uvc_running(void);
bool usb_uvc_streaming(void);

/* Hands the USB pins back to the USB-Serial/JTAG controller (flashing + logs). */
void usb_restore_serial_jtag(void);
void usb_log_phy_state(void);
