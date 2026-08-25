#include "csi_udp.h"
#include "csi_frame.h"

#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "csi_udp";

#define CSI_QUEUE_LEN 32
#define CSI_TASK_STACK 4096
#define CSI_TASK_PRIO 5

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_running;
static uint32_t s_send_ok;
static uint32_t s_send_fail;
static uint32_t s_drop;

static void pack_and_send(int sock, const struct sockaddr_in *dest, const csi_sample_t *s)
{
    uint8_t buf[CSI1_HDR_LEN + CSI1_MAX_IQ];
    csi1_header_t *hdr = (csi1_header_t *)buf;

    uint16_t len = s->len;
    if (len > CSI1_MAX_IQ) {
        len = CSI1_MAX_IQ;
    }

    hdr->magic = CSI1_MAGIC;
    hdr->version = CSI1_VERSION;
    hdr->node_id = (uint8_t)CONFIG_CSI_NODE_ID;
    hdr->seq = s->seq;
    hdr->t_us = s->t_us;
    hdr->rssi = s->rssi;
    hdr->noise_floor = s->noise_floor;
    hdr->channel = s->channel;
    memcpy(hdr->mac, s->mac, 6);
    hdr->len = len;
    memcpy(buf + CSI1_HDR_LEN, s->iq, len);

    const size_t total = (size_t)CSI1_HDR_LEN + len;
    const ssize_t n = sendto(sock, buf, total, 0, (const struct sockaddr *)dest, sizeof(*dest));
    if (n == (ssize_t)total) {
        s_send_ok++;
    } else {
        s_send_fail++;
    }
}

static void csi_udp_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_CSI_COLLECTOR_PORT);
    dest.sin_addr.s_addr = inet_addr(CONFIG_CSI_COLLECTOR_IP);

    ESP_LOGI(TAG, "uplink -> %s:%d", CONFIG_CSI_COLLECTOR_IP, CONFIG_CSI_COLLECTOR_PORT);

    csi_sample_t sample;
    uint32_t last_log_ms = 0;

    while (s_running) {
        if (xQueueReceive(s_queue, &sample, pdMS_TO_TICKS(500)) == pdTRUE) {
            pack_and_send(sock, &dest, &sample);
        }

        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_log_ms >= 5000) {
            last_log_ms = now;
            ESP_LOGI(TAG, "send_ok=%lu send_fail=%lu drop=%lu q=%u",
                     (unsigned long)s_send_ok,
                     (unsigned long)s_send_fail,
                     (unsigned long)s_drop,
                     (unsigned)uxQueueMessagesWaiting(s_queue));
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

esp_err_t csi_udp_start(void)
{
    if (s_queue) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(CSI_QUEUE_LEN, sizeof(csi_sample_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    s_send_ok = 0;
    s_send_fail = 0;
    s_drop = 0;

    BaseType_t ok = xTaskCreate(csi_udp_task, "csi_udp", CSI_TASK_STACK, NULL, CSI_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void csi_udp_stop(void)
{
    s_running = false;
}

bool csi_udp_enqueue(const csi_sample_t *sample)
{
    if (!s_queue || !sample) {
        return false;
    }
    if (xQueueSend(s_queue, sample, 0) != pdTRUE) {
        s_drop++;
        return false;
    }
    return true;
}
