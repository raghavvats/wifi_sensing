# SCHED control plane

UDP packets between the Raspberry Pi orchestrator and ESP32 CSI nodes. All multi-byte integers are **little-endian**.

ESPs act only on SCHED messages for TDMA; they do not self-clock slots. ESPs also announce identity via **HELLO** so the Pi can build SYNC using each board’s firmware `CONFIG_CSI_NODE_ID`.

## Common header

| Offset | Field   | Type | Notes                                      |
|--------|---------|------|--------------------------------------------|
| 0      | magic   | `u32`| `0x53434844` (`SCHD` in ASCII)             |
| 4      | version | `u8` | `1`                                        |
| 5      | type    | `u8` | Message type (see below)                   |

**Header size before type-specific body:** 6 bytes

## Message types

| `type` | Name         | Direction | Purpose                                      |
|--------|--------------|-----------|----------------------------------------------|
| `1`    | `SLOT_BEGIN` | Pi → ESP  | Start of a TDMA slot                         |
| `2`    | `SYNC`       | Pi → ESP  | Session start: timing + peer map             |
| `3`    | `HELLO`      | ESP → Pi  | Announce `node_id` + STA MAC + IPv4          |

## `SLOT_BEGIN` (type=1)

| Offset | Field     | Type | Notes                                      |
|--------|-----------|------|--------------------------------------------|
| 6      | cycle_id  | `u32`| Monotonic mesh cycle counter               |
| 10     | slot_idx  | `u8` | Slot index within the cycle                |
| 11     | tx_node   | `u8` | Node allowed to transmit (`0` = Pi)        |
| 12     | phase     | `u8` | `0` = PROBE, `1` = REPORT                  |
| 13     | rx_target | `u8` | Pi PROBE: intended ESP `node_id`. `0` = any (legacy). ESP TX / REPORT: `0`. |

**Total size:** 14 bytes

### Actions

- **PROBE + `tx_node` = Pi (0):** Pi unicasts a probe on `:5005` to the slot’s target ESP (`rx_target`). Only that ESP keeps **one** CSI sample whose TX MAC is the AP BSSID.
- **PROBE + `tx_node` = ESP id:** That ESP unicasts one UDP probe to the Pi on `:5005`. Peers keep **one** CSI sample whose TX MAC is that ESP’s STA MAC (overhear).
- **REPORT + `tx_node` = ESP id:** That ESP flushes buffered CSI2 records to `:5006`.

Hard rule: no CSI uplink (`:5006`) during PROBE slots. CSI is accepted only from the **scheduled transmitter MAC** for the current slot.

## `SYNC` (type=2)

| Offset | Field            | Type     | Notes                          |
|--------|------------------|----------|--------------------------------|
| 6      | probe_slot_ms    | `u16`    | Default `20`                   |
| 8      | report_slot_ms   | `u16`    | Default `40`                   |
| 10     | n_peers          | `u8`     | Number of peer entries         |
| 11     | pad              | `u8`     | `0`                            |
| 12     | peers[]          | variable | `n_peers` × peer entry         |

### Peer entry (12 bytes each)

| Offset | Field   | Type    | Notes                |
|--------|---------|---------|----------------------|
| 0      | node_id | `u8`    | Firmware node id     |
| 1      | mac     | `u8[6]` | STA MAC              |
| 7      | ipv4    | `u8[4]` | IPv4 network order   |
| 11     | pad     | `u8`    | `0`                  |

Pi (`node_id=0`) is omitted; ESPs already know the AP BSSID from association.

## `HELLO` (type=3)

ESP → Pi on UDP **5008** (default), about once per second until powered down.

| Offset | Field   | Type    | Notes                         |
|--------|---------|---------|-------------------------------|
| 6      | node_id | `u8`    | `CONFIG_CSI_NODE_ID`          |
| 7      | mac     | `u8[6]` | STA MAC                       |
| 13     | ipv4    | `u8[4]` | Station IPv4 (network order)  |
| 17     | pad     | `u8`    | `0`                           |

**Total size:** 18 bytes

The orchestrator collects HELLOs, builds the peer map keyed by firmware `node_id`, then sends `SYNC`.

## Schedule table (3 ESP nodes)

| Slot | Phase  | `tx_node` | Measurements              |
|------|--------|-----------|---------------------------|
| 0    | PROBE  | 0 (Pi)    | Pi→A                      |
| 1    | PROBE  | 0 (Pi)    | Pi→B                      |
| 2    | PROBE  | 0 (Pi)    | Pi→C                      |
| 3    | PROBE  | A         | A→B, A→C (overhear)       |
| 4    | PROBE  | B         | B→A, B→C (overhear)       |
| 5    | PROBE  | C         | C→A, C→B (overhear)       |
| 6    | REPORT | A         | A batch CSI uplink        |
| 7    | REPORT | B         | B batch CSI uplink        |
| 8    | REPORT | C         | C batch CSI uplink        |

Pi slot targets are ordered by ascending ESP `node_id`.

## Constants

```
SCHED_MAGIC           = 0x53434844
SCHED_VERSION         = 1
SCHED_TYPE_SLOT_BEGIN = 1
SCHED_TYPE_SYNC       = 2
SCHED_TYPE_HELLO      = 3
SCHED_HDR_COMMON      = 6
SCHED_SLOT_BEGIN_LEN  = 14
SCHED_HELLO_LEN       = 18
MESH_CONTROL_PORT     = 5007
MESH_HELLO_PORT       = 5008
PHASE_PROBE           = 0
PHASE_REPORT          = 1
NODE_ID_PI            = 0
```

## Ports

| Role                         | UDP port |
|------------------------------|----------|
| Probes (Pi↔ESP)              | 5005     |
| CSI uplink (ESP→Pi)          | 5006     |
| SCHED control (Pi→ESP)       | 5007     |
| HELLO announce (ESP→Pi)      | 5008     |
