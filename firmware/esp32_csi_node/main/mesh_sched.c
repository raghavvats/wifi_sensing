#include "mesh_sched.h"
#include "sched.h"
#include "csi_udp.h"

#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "mesh_sched";

typedef struct {
    uint8_t node_id;
    uint8_t mac[6];
    uint8_t ipv4[4];
    bool    valid;
} peer_t;

static peer_t s_peers[MESH_MAX_PEERS];
static uint8_t s_n_peers;
static uint8_t s_ap_bssid[6];
static bool s_have_ap;

static volatile bool s_probe_active;
static volatile uint32_t s_cycle_id;
static volatile uint8_t s_slot_idx;
static volatile uint8_t s_tx_node;
static volatile uint8_t s_rx_target;
static volatile uint32_t s_last_probe_seq;
static volatile uint32_t s_kept_cycle;
static volatile uint8_t s_kept_slot;
static volatile bool s_slot_taken;

static int s_probe_tx_sock = -1;
static uint32_t s_local_probe_seq;

static void clear_peers(void)
{
    memset(s_peers, 0, sizeof(s_peers));
    s_n_peers = 0;
}

static bool lookup_peer_mac(uint8_t node_id, uint8_t out_mac[6])
{
    for (uint8_t i = 0; i < s_n_peers; i++) {
        if (s_peers[i].valid && s_peers[i].node_id == node_id) {
            memcpy(out_mac, s_peers[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void apply_sync(const uint8_t *buf, size_t len)
{
    if (len < SCHED_SYNC_FIXED_LEN) {
        return;
    }
    const sched_sync_t *sync = (const sched_sync_t *)buf;
    if (sync->n_peers > MESH_MAX_PEERS) {
        ESP_LOGW(TAG, "SYNC n_peers=%u truncated", sync->n_peers);
    }

    clear_peers();
    const size_t need = SCHED_SYNC_FIXED_LEN + (size_t)sync->n_peers * SCHED_PEER_ENTRY_LEN;
    if (len < need) {
        ESP_LOGW(TAG, "SYNC short (%u < %u)", (unsigned)len, (unsigned)need);
        return;
    }

    const uint8_t n = sync->n_peers <= MESH_MAX_PEERS ? sync->n_peers : MESH_MAX_PEERS;
    const sched_peer_t *peers = (const sched_peer_t *)(buf + SCHED_SYNC_FIXED_LEN);
    for (uint8_t i = 0; i < n; i++) {
        s_peers[i].node_id = peers[i].node_id;
        memcpy(s_peers[i].mac, peers[i].mac, 6);
        memcpy(s_peers[i].ipv4, peers[i].ipv4, 4);
        s_peers[i].valid = true;
    }
    s_n_peers = n;

    csi_udp_set_defer(true);
    ESP_LOGI(TAG, "SYNC peers=%u probe_ms=%u report_ms=%u",
             (unsigned)s_n_peers, sync->probe_slot_ms, sync->report_slot_ms);
    for (uint8_t i = 0; i < s_n_peers; i++) {
        ESP_LOGI(TAG, "  peer id=%u mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 s_peers[i].node_id,
                 s_peers[i].mac[0], s_peers[i].mac[1], s_peers[i].mac[2],
                 s_peers[i].mac[3], s_peers[i].mac[4], s_peers[i].mac[5]);
    }
}

static void send_mesh_probe(void)
{
    if (s_probe_tx_sock < 0) {
        return;
    }

    s_local_probe_seq++;
    s_last_probe_seq = s_local_probe_seq;

    uint8_t payload[32];
    memset(payload, 0, sizeof(payload));
    uint32_t seq = s_local_probe_seq;
    uint64_t t_us = (uint64_t)esp_timer_get_time();
    memcpy(payload, &seq, 4);
    memcpy(payload + 4, &t_us, 8);

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_CSI_PROBE_PORT);
    dest.sin_addr.s_addr = inet_addr(CONFIG_CSI_COLLECTOR_IP);

    const ssize_t n = sendto(s_probe_tx_sock, payload, sizeof(payload), 0,
                             (const struct sockaddr *)&dest, sizeof(dest));
    if (n < 0) {
        ESP_LOGW(TAG, "probe TX failed");
    }
}

static void send_hello(int sock)
{
    sched_hello_t hello = {0};
    hello.magic = SCHED_MAGIC;
    hello.version = SCHED_VERSION;
    hello.type = SCHED_TYPE_HELLO;
    hello.node_id = (uint8_t)CONFIG_CSI_NODE_ID;

    esp_wifi_get_mac(WIFI_IF_STA, hello.mac);

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            memcpy(hello.ipv4, &ip.ip.addr, 4);
        }
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_CSI_HELLO_PORT);
    dest.sin_addr.s_addr = inet_addr(CONFIG_CSI_COLLECTOR_IP);

    (void)sendto(sock, &hello, sizeof(hello), 0, (const struct sockaddr *)&dest, sizeof(dest));
}

static void handle_slot_begin(const sched_slot_begin_t *msg)
{
    s_cycle_id = msg->cycle_id;
    s_slot_idx = msg->slot_idx;
    s_tx_node = msg->tx_node;
    s_rx_target = msg->rx_target;
    s_slot_taken = false;

    if (msg->slot_idx == 0 && msg->phase == PHASE_PROBE) {
        csi_udp_clear_buffer();
    }

    if (msg->phase == PHASE_PROBE) {
        s_probe_active = true;
        if (msg->tx_node == (uint8_t)CONFIG_CSI_NODE_ID) {
            send_mesh_probe();
        }
    } else if (msg->phase == PHASE_REPORT) {
        s_probe_active = false;
        if (msg->tx_node == (uint8_t)CONFIG_CSI_NODE_ID) {
            csi_udp_flush();
        }
    } else {
        s_probe_active = false;
    }
}

static void mesh_sched_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "sched socket failed");
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFIG_CSI_CONTROL_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "sched bind :%d failed", CONFIG_CSI_CONTROL_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    s_probe_tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_probe_tx_sock < 0) {
        ESP_LOGW(TAG, "probe TX socket failed");
    }

    int hello_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (hello_sock < 0) {
        ESP_LOGW(TAG, "HELLO socket failed");
    }

    ESP_LOGI(TAG, "listening for SCHED on UDP :%d; HELLO -> :%d",
             CONFIG_CSI_CONTROL_PORT, CONFIG_CSI_HELLO_PORT);

    /* Announce identity immediately so Pi can discover firmware node_id. */
    if (hello_sock >= 0) {
        send_hello(hello_sock);
    }

    uint8_t buf[256];
    uint32_t last_hello_ms = 0;

    for (;;) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (hello_sock >= 0 && (now_ms - last_hello_ms) >= 1000) {
            last_hello_ms = now_ms;
            send_hello(hello_sock);
        }

        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        const ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < (ssize_t)SCHED_HDR_COMMON) {
            continue;
        }

        const sched_common_t *common = (const sched_common_t *)buf;
        if (common->magic != SCHED_MAGIC || common->version != SCHED_VERSION) {
            continue;
        }

        if (common->type == SCHED_TYPE_SYNC) {
            apply_sync(buf, (size_t)n);
        } else if (common->type == SCHED_TYPE_SLOT_BEGIN) {
            if (n < (ssize_t)SCHED_SLOT_BEGIN_LEN) {
                continue;
            }
            handle_slot_begin((const sched_slot_begin_t *)buf);
        }
    }
}

esp_err_t mesh_sched_start(const uint8_t ap_bssid[6])
{
    if (ap_bssid) {
        memcpy(s_ap_bssid, ap_bssid, 6);
        s_have_ap = true;
    }

#if CONFIG_CSI_MESH_TDMA
    csi_udp_set_defer(true);
    BaseType_t ok = xTaskCreate(mesh_sched_task, "mesh_sched", 6144, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mesh TDMA enabled (node_id=%d)", CONFIG_CSI_NODE_ID);
#else
    csi_udp_set_defer(false);
    ESP_LOGI(TAG, "mesh TDMA disabled — legacy immediate CSI1 uplink");
#endif
    return ESP_OK;
}

bool mesh_mac_allowed(const uint8_t mac[6])
{
    if (!mac) {
        return false;
    }
    if (s_have_ap && memcmp(mac, s_ap_bssid, 6) == 0) {
        return true;
    }
    for (uint8_t i = 0; i < s_n_peers; i++) {
        if (s_peers[i].valid && memcmp(mac, s_peers[i].mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

bool mesh_mac_is_scheduled_tx(const uint8_t mac[6])
{
    if (!mac || !s_probe_active) {
        return false;
    }

    const uint8_t tx = s_tx_node;
    if (tx == NODE_ID_PI) {
        /* Pi probe slots name an rx_target; only that ESP should keep CSI.
         * rx_target==0 keeps legacy "any listener" behavior. */
        if (s_rx_target != 0 && s_rx_target != (uint8_t)CONFIG_CSI_NODE_ID) {
            return false;
        }
        return s_have_ap && memcmp(mac, s_ap_bssid, 6) == 0;
    }

    uint8_t peer_mac[6];
    if (!lookup_peer_mac(tx, peer_mac)) {
        return false;
    }
    return memcmp(mac, peer_mac, 6) == 0;
}

uint8_t mesh_tx_node_for_mac(const uint8_t mac[6])
{
    if (!mac) {
        return 255;
    }
    if (s_have_ap && memcmp(mac, s_ap_bssid, 6) == 0) {
        return NODE_ID_PI;
    }
    for (uint8_t i = 0; i < s_n_peers; i++) {
        if (s_peers[i].valid && memcmp(mac, s_peers[i].mac, 6) == 0) {
            return s_peers[i].node_id;
        }
    }
    return 255;
}

bool mesh_probe_active(void)
{
    return s_probe_active;
}

uint32_t mesh_current_cycle_id(void)
{
    return s_cycle_id;
}

uint8_t mesh_current_slot_idx(void)
{
    return s_slot_idx;
}

uint8_t mesh_current_tx_node(void)
{
    return s_tx_node;
}

uint8_t mesh_current_rx_target(void)
{
    return s_rx_target;
}

uint32_t mesh_last_probe_seq(void)
{
    return s_last_probe_seq;
}

void mesh_note_probe_seq(uint32_t seq)
{
    s_last_probe_seq = seq;
}

bool mesh_slot_sample_taken(void)
{
    return s_slot_taken && s_kept_cycle == s_cycle_id && s_kept_slot == s_slot_idx;
}

void mesh_mark_slot_sample_taken(void)
{
    s_kept_cycle = s_cycle_id;
    s_kept_slot = s_slot_idx;
    s_slot_taken = true;
}
