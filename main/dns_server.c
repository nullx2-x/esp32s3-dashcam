#include "dns_server.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define DNS_PORT     53
#define DNS_MAX_LEN  512
#define QTYPE_A      1
#define QCLASS_IN    1

static const char *TAG = "dns";
static uint32_t s_answer_ip;

static int build_reply(uint8_t *pkt, int len)
{
    if (len < 12) return -1;
    if (pkt[2] & 0x80) return -1;               /* not a query */
    if (((pkt[2] >> 3) & 0x0F) != 0) return -1; /* only standard queries */
    if (((pkt[4] << 8) | pkt[5]) < 1) return -1;

    int pos = 12;
    while (pos < len && pkt[pos] != 0) {
        if ((pkt[pos] & 0xC0) != 0) return -1;
        pos += pkt[pos] + 1;
    }
    pos++;
    if (pos + 4 > len) return -1;
    const uint16_t qtype = (pkt[pos] << 8) | pkt[pos + 1];
    const uint16_t qclass = (pkt[pos + 2] << 8) | pkt[pos + 3];
    pos += 4;

    const bool answer = qtype == QTYPE_A && (qclass & 0x7FFF) == QCLASS_IN;
    pkt[2] = 0x84 | (pkt[2] & 0x01); /* response, authoritative, keep RD */
    pkt[3] = 0x00;
    pkt[4] = 0;
    pkt[5] = 1;
    pkt[6] = 0;
    pkt[7] = answer ? 1 : 0;
    memset(pkt + 8, 0, 4);
    if (!answer) return pos;
    if (pos + 16 > DNS_MAX_LEN) return -1;

    uint8_t *a = pkt + pos;
    a[0] = 0xC0; a[1] = 0x0C;              /* name: pointer to question */
    a[2] = 0; a[3] = QTYPE_A;
    a[4] = 0; a[5] = QCLASS_IN;
    a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 60; /* TTL */
    a[10] = 0; a[11] = 4;
    memcpy(a + 12, &s_answer_ip, 4);
    return pos + 16;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = s_answer_ip,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS running");

    uint8_t pkt[DNS_MAX_LEN];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) continue;
        int reply = build_reply(pkt, len);
        if (reply > 0) sendto(sock, pkt, reply, 0, (struct sockaddr *)&from, from_len);
    }
}

void dns_server_start(uint32_t ap_ip_addr)
{
    s_answer_ip = ap_ip_addr;
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
}
