#include "csi_udp.h"
#include "csi_frame.h"

#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "csi_udp";

#define CSI_QUEUE_LEN 32
#define CSI_BUF_LEN 16
#define CSI_TASK_STACK 6144
#define CSI_TASK_PRIO 5

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_running;
static volatile bool s_defer;
static uint32_t s_send_ok;
static uint32_t s_send_fail;
static uint32_t s_drop;

static csi_sample_t s_buf[CSI_BUF_LEN];
static uint8_t s_buf_count;
static SemaphoreHandle_t s_buf_mu;

/* Shared by csi_udp task (CSI1) and synchronous CSI2 flush from mesh_sched. */
static int s_sock = -1;
static struct sockaddr_in s_dest;

/* Shared TX scratch — never on task stack (CSI2 packet up to ~552 B). */
static uint8_t s_tx_pkt[CSI2_HDR_LEN + CSI_MAX_IQ];

static void pack_and_send_csi1(int sock, const struct sockaddr_in *dest, const csi_sample_t *s)
{
    csi1_header_t *hdr = (csi1_header_t *)s_tx_pkt;

    uint16_t len = s->len;
    if (len > CSI_MAX_IQ) {
        len = CSI_MAX_IQ;
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
    memcpy(s_tx_pkt + CSI1_HDR_LEN, s->iq, len);

    const size_t total = (size_t)CSI1_HDR_LEN + len;
    const ssize_t n = sendto(sock, s_tx_pkt, total, 0, (const struct sockaddr *)dest, sizeof(*dest));
    if (n == (ssize_t)total) {
        s_send_ok++;
    } else {
        s_send_fail++;
    }
}

static void pack_and_send_csi2(int sock, const struct sockaddr_in *dest, const csi_sample_t *s)
{
    csi2_header_t *hdr = (csi2_header_t *)s_tx_pkt;

    uint16_t len = s->len;
    if (len > CSI_MAX_IQ) {
        len = CSI_MAX_IQ;
    }

    hdr->magic = CSI2_MAGIC;
    hdr->version = CSI2_VERSION;
    hdr->node_id = (uint8_t)CONFIG_CSI_NODE_ID;
    hdr->seq = s->seq;
    hdr->t_us = s->t_us;
    hdr->rssi = s->rssi;
    hdr->noise_floor = s->noise_floor;
    hdr->channel = s->channel;
    memcpy(hdr->mac, s->mac, 6);
    hdr->cycle_id = s->cycle_id;
    hdr->slot_idx = s->slot_idx;
    hdr->tx_node_id = s->tx_node_id;
    hdr->rx_node_id = s->rx_node_id ? s->rx_node_id : (uint8_t)CONFIG_CSI_NODE_ID;
    hdr->probe_seq = s->probe_seq;
    hdr->len = len;
    memcpy(s_tx_pkt + CSI2_HDR_LEN, s->iq, len);

    const size_t total = (size_t)CSI2_HDR_LEN + len;
    const ssize_t n = sendto(sock, s_tx_pkt, total, 0, (const struct sockaddr *)dest, sizeof(*dest));
    if (n == (ssize_t)total) {
        s_send_ok++;
    } else {
        s_send_fail++;
    }
}

static void flush_buffer_locked(int sock, const struct sockaddr_in *dest)
{
    /* Send one-by-one from s_buf; do not copy the whole buffer onto the stack. */
    uint8_t n = s_buf_count;
    s_buf_count = 0;
    for (uint8_t i = 0; i < n; i++) {
        pack_and_send_csi2(sock, dest, &s_buf[i]);
    }
    if (n > 0) {
        ESP_LOGI(TAG, "flushed %u CSI2 frames", (unsigned)n);
    }
}

static void csi_udp_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "uplink -> %s:%d", CONFIG_CSI_COLLECTOR_IP, CONFIG_CSI_COLLECTOR_PORT);

    csi_sample_t sample;
    uint32_t last_log_ms = 0;

    while (s_running) {
        if (xQueueReceive(s_queue, &sample, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_sock >= 0 && s_buf_mu &&
                xSemaphoreTake(s_buf_mu, pdMS_TO_TICKS(20)) == pdTRUE) {
                pack_and_send_csi1(s_sock, &s_dest, &sample);
                xSemaphoreGive(s_buf_mu);
            }
        }

        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_log_ms >= 5000) {
            last_log_ms = now;
            ESP_LOGI(TAG, "send_ok=%lu send_fail=%lu drop=%lu buf=%u defer=%d",
                     (unsigned long)s_send_ok,
                     (unsigned long)s_send_fail,
                     (unsigned long)s_drop,
                     (unsigned)s_buf_count,
                     (int)s_defer);
        }
    }

    vTaskDelete(NULL);
}

esp_err_t csi_udp_start(void)
{
    if (s_queue) {
        return ESP_OK;
    }

    s_buf_mu = xSemaphoreCreateMutex();
    if (!s_buf_mu) {
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(CSI_QUEUE_LEN, sizeof(csi_sample_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(CONFIG_CSI_COLLECTOR_PORT);
    s_dest.sin_addr.s_addr = inet_addr(CONFIG_CSI_COLLECTOR_IP);

    s_running = true;
    s_defer = false;
    s_buf_count = 0;
    s_send_ok = 0;
    s_send_fail = 0;
    s_drop = 0;

    BaseType_t ok = xTaskCreate(csi_udp_task, "csi_udp", CSI_TASK_STACK, NULL, CSI_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        close(s_sock);
        s_sock = -1;
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

void csi_udp_set_defer(bool defer)
{
    s_defer = defer;
}

bool csi_udp_enqueue(const csi_sample_t *sample)
{
    if (!sample) {
        return false;
    }

    if (s_defer) {
        if (!s_buf_mu) {
            return false;
        }
        if (xSemaphoreTake(s_buf_mu, 0) != pdTRUE) {
            s_drop++;
            return false;
        }
        if (s_buf_count >= CSI_BUF_LEN) {
            s_drop++;
            xSemaphoreGive(s_buf_mu);
            return false;
        }
        s_buf[s_buf_count++] = *sample;
        xSemaphoreGive(s_buf_mu);
        return true;
    }

    if (!s_queue) {
        return false;
    }
    if (xQueueSend(s_queue, sample, 0) != pdTRUE) {
        s_drop++;
        return false;
    }
    return true;
}

void csi_udp_clear_buffer(void)
{
    if (!s_buf_mu) {
        return;
    }
    if (xSemaphoreTake(s_buf_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_buf_count = 0;
        xSemaphoreGive(s_buf_mu);
    }
}

uint32_t csi_udp_buffered_count(void)
{
    return s_buf_count;
}

void csi_udp_flush(void)
{
    /*
     * Flush synchronously under the socket/buffer mutex.
     * Async flush raced the next cycle's clear_buffer(): the last REPORT
     * node (highest node_id) only had report_slot_ms before clear, while the
     * old task polled every 50ms — so ~half of node-3 uplinks were wiped.
     */
    if (s_sock < 0 || !s_buf_mu) {
        return;
    }
    if (xSemaphoreTake(s_buf_mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        flush_buffer_locked(s_sock, &s_dest);
        xSemaphoreGive(s_buf_mu);
    }
}
