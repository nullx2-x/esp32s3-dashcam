#pragma once

#include <stdint.h>

/* Answers every A query on the access point with the dashcam's own address. */
void dns_server_start(uint32_t ap_ip_addr);
