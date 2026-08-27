#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_MAGIC              0x53434844u  /* 'SCHD' */
#define SCHED_VERSION            1
#define SCHED_TYPE_SLOT_BEGIN    1
#define SCHED_TYPE_SYNC          2
#define SCHED_TYPE_HELLO         3
#define SCHED_HDR_COMMON         6
#define SCHED_SLOT_BEGIN_LEN     14
#define SCHED_SYNC_FIXED_LEN     12
#define SCHED_PEER_ENTRY_LEN     12
#define SCHED_HELLO_LEN          18

#define PHASE_PROBE              0
#define PHASE_REPORT             1
#define NODE_ID_PI               0

#define MESH_MAX_PEERS           8

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
} sched_common_t;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint32_t cycle_id;
    uint8_t  slot_idx;
    uint8_t  tx_node;
    uint8_t  phase;
    uint8_t  rx_target; /* Pi PROBE: intended RX node_id; 0 = any / unused */
} sched_slot_begin_t;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t probe_slot_ms;
    uint16_t report_slot_ms;
    uint8_t  n_peers;
    uint8_t  pad;
    /* peer entries follow */
} sched_sync_t;

typedef struct {
    uint8_t node_id;
    uint8_t mac[6];
    uint8_t ipv4[4];
    uint8_t pad;
} sched_peer_t;

/** ESP → Pi identity announce (firmware CONFIG_CSI_NODE_ID + STA MAC + IP). */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  node_id;
    uint8_t  mac[6];
    uint8_t  ipv4[4];
    uint8_t  pad;
} sched_hello_t;
#pragma pack(pop)

_Static_assert(sizeof(sched_slot_begin_t) == SCHED_SLOT_BEGIN_LEN, "SLOT_BEGIN size");
_Static_assert(sizeof(sched_sync_t) == SCHED_SYNC_FIXED_LEN, "SYNC fixed size");
_Static_assert(sizeof(sched_peer_t) == SCHED_PEER_ENTRY_LEN, "peer entry size");
_Static_assert(sizeof(sched_hello_t) == SCHED_HELLO_LEN, "HELLO size");

#ifdef __cplusplus
}
#endif
