#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t seq;
    uint64_t t_us;
    int8_t   rssi;
    int8_t   noise_floor;
    uint8_t  channel;
    uint8_t  mac[6];
    uint16_t len;
    int8_t   iq[512];
    /* Mesh TDMA metadata (CSI2) */
    uint32_t cycle_id;
    uint8_t  slot_idx;
    uint8_t  tx_node_id;
    uint8_t  rx_node_id;
    uint32_t probe_seq;
} csi_sample_t;

esp_err_t csi_udp_start(void);
void csi_udp_stop(void);

/** When true, samples are buffered until csi_udp_flush(); no immediate send. */
void csi_udp_set_defer(bool defer);

/** Non-blocking enqueue. Returns false if dropped. */
bool csi_udp_enqueue(const csi_sample_t *sample);

/** Synchronously send all buffered samples as CSI2 (mesh report slot). */
void csi_udp_flush(void);

/** Clear buffer without sending (start of new cycle). */
void csi_udp_clear_buffer(void);

uint32_t csi_udp_buffered_count(void);

#ifdef __cplusplus
}
#endif
