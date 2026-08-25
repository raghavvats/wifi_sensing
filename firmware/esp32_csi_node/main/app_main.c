#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "csi_udp.h"

static const char *TAG = "csi_node";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry;
static uint8_t s_ap_bssid[6];
static bool s_have_bssid;
static uint32_t s_csi_seq;
static int64_t s_last_csi_us;
static uint32_t s_csi_cb_count;
static uint32_t s_csi_filt_drop;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_have_bssid = false;
        if (s_retry < 20) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "retry connect (%d)", s_retry);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_CSI_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, CONFIG_CSI_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Disable power save so CSI timing stays more consistent. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "connecting to SSID:%s", CONFIG_CSI_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected");
    } else {
        ESP_LOGE(TAG, "failed to connect");
        abort();
    }
}

static void cache_ap_bssid(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(s_ap_bssid, ap.bssid, 6);
        s_have_bssid = true;
        ESP_LOGI(TAG, "AP BSSID %02x:%02x:%02x:%02x:%02x:%02x ch=%d",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5],
                 ap.primary);
    }
}

static void wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)buf;
    (void)type;
}

static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;

    if (!info || !info->buf || info->len <= 0) {
        return;
    }

#if CONFIG_CSI_FILTER_BSSID
    if (s_have_bssid && memcmp(info->mac, s_ap_bssid, 6) != 0) {
        s_csi_filt_drop++;
        return;
    }
#endif

    const int64_t now = esp_timer_get_time();
    const int64_t min_interval_us = 1000000LL / CONFIG_CSI_MAX_RATE_HZ;
    if ((now - s_last_csi_us) < min_interval_us) {
        return;
    }
    s_last_csi_us = now;

    csi_sample_t sample = {0};
    sample.seq = ++s_csi_seq;
    sample.t_us = (uint64_t)now;
    sample.rssi = info->rx_ctrl.rssi;
#if defined(CONFIG_IDF_TARGET_ESP32)
    sample.noise_floor = info->rx_ctrl.noise_floor;
#else
    sample.noise_floor = 0;
#endif
    sample.channel = info->rx_ctrl.channel;
    memcpy(sample.mac, info->mac, 6);

    uint16_t len = (uint16_t)info->len;
    if (len > sizeof(sample.iq)) {
        len = sizeof(sample.iq);
    }
    sample.len = len;
    memcpy(sample.iq, info->buf, len);

    if (csi_udp_enqueue(&sample)) {
        s_csi_cb_count++;
    }
}

static void csi_init(void)
{
    cache_ap_bssid();

    ESP_ERROR_CHECK(csi_udp_start());

    /*
     * Classic ESP32 CSI config (ESP-IDF). Prefer LLTF for router compatibility.
     * Start CSI before heavy probe traffic (classic ESP32 quirk).
     */
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = false,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = 0,
    };

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    /* Promiscuous RX increases CSI opportunities while associated. */
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_LOGI(TAG, "CSI enabled (node_id=%d max_hz=%d filter_bssid=%d)",
             CONFIG_CSI_NODE_ID,
             CONFIG_CSI_MAX_RATE_HZ,
             CONFIG_CSI_FILTER_BSSID);
}

static void probe_sink_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "probe socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFIG_CSI_PROBE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "probe bind :%d failed", CONFIG_CSI_PROBE_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening for probes on UDP :%d", CONFIG_CSI_PROBE_PORT);

    uint8_t buf[64];
    uint32_t probes = 0;
    uint32_t last_log_ms = 0;

    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        const ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            probes++;
        }

        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_log_ms >= 5000) {
            last_log_ms = now;
            ESP_LOGI(TAG, "probes=%lu csi_queued=%lu filt_drop=%lu",
                     (unsigned long)probes,
                     (unsigned long)s_csi_cb_count,
                     (unsigned long)s_csi_filt_drop);
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_sta();
    csi_init();

    xTaskCreate(probe_sink_task, "probe_sink", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "ready — start Pi probes AFTER this message");
}
