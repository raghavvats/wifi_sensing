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
} csi_sample_t;

esp_err_t csi_udp_start(void);
void csi_udp_stop(void);

/** Non-blocking enqueue from CSI callback. Returns false if dropped. */
bool csi_udp_enqueue(const csi_sample_t *sample);

#ifdef __cplusplus
}
#endif
