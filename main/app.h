#pragma once

#include <stdbool.h>

/* Finishes the current recording, then restarts. Maintenance mode boots once
 * without the USB webcam so the USB port works for flashing and logs. */
void app_request_reboot(bool maintenance);
bool app_is_maintenance(void);
const char *app_version(void);
