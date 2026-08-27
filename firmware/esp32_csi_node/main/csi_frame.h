#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSI1_MAGIC      0x43534931u  /* 'CSI1' */
#define CSI1_VERSION    1
#define CSI1_HDR_LEN    29

#define CSI2_MAGIC      0x43534932u  /* 'CSI2' */
#define CSI2_VERSION    2
#define CSI2_HDR_LEN    40

#define CSI_MAX_IQ      512

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  node_id;
    uint32_t seq;
    uint64_t t_us;
    int8_t   rssi;
    int8_t   noise_floor;
    uint8_t  channel;
    uint8_t  mac[6];
    uint16_t len;
    /* int8_t iq[len] follows */
} csi1_header_t;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  node_id;
    uint32_t seq;
    uint64_t t_us;
    int8_t   rssi;
    int8_t   noise_floor;
    uint8_t  channel;
    uint8_t  mac[6];
    uint32_t cycle_id;
    uint8_t  slot_idx;
    uint8_t  tx_node_id;
    uint8_t  rx_node_id;
    uint32_t probe_seq;
    uint16_t len;
    /* int8_t iq[len] follows */
} csi2_header_t;
#pragma pack(pop)

_Static_assert(sizeof(csi1_header_t) == CSI1_HDR_LEN, "CSI1 header size mismatch");
_Static_assert(sizeof(csi2_header_t) == CSI2_HDR_LEN, "CSI2 header size mismatch");

#ifdef __cplusplus
}
#endif
