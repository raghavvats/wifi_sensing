# CSI uplink frames (CSI1 / CSI2)

UDP payloads from each ESP32 CSI node to the Raspberry Pi collector. All multi-byte integers are **little-endian**.

## CSI1 (legacy star topology)

Immediate per-sample uplink used by the original probe sender. Still decoded by the collector for backward compatibility.

| Offset | Field         | Type     | Notes                                      |
|--------|---------------|----------|--------------------------------------------|
| 0      | magic         | `u32`    | `0x43534931` (`CSI1` in ASCII)             |
| 4      | version       | `u8`     | `1`                                        |
| 5      | node_id       | `u8`     | Reporting node (compile-time / NVS)        |
| 6      | seq           | `u32`    | Monotonic CSI frame counter                |
| 10     | t_us          | `u64`    | ESP `esp_timer_get_time()` microseconds    |
| 18     | rssi          | `i8`     | From `wifi_pkt_rx_ctrl_t.rssi`             |
| 19     | noise_floor   | `i8`     | From RX ctrl when available, else `0`      |
| 20     | channel       | `u8`     | Primary channel                            |
| 21     | mac           | `u8[6]`  | Transmitter MAC                             |
| 27     | len           | `u16`    | Length of `iq[]` in bytes                  |
| 29     | iq            | `i8[len]`| CSI I/Q samples                            |

**Header size:** 29 bytes  
**Max `len`:** 512 (firmware truncates if larger)

## CSI2 (mesh TDMA)

Batched / deferred uplink for mesh orchestration. Same I/Q encoding as CSI1, plus link labels.

| Offset | Field         | Type     | Notes                                      |
|--------|---------------|----------|--------------------------------------------|
| 0      | magic         | `u32`    | `0x43534932` (`CSI2` in ASCII)             |
| 4      | version       | `u8`     | `2`                                        |
| 5      | node_id       | `u8`     | Reporting node (always the measuring RX)   |
| 6      | seq           | `u32`    | Monotonic CSI frame counter on this node   |
| 10     | t_us          | `u64`    | Capture time (`esp_timer_get_time`)        |
| 18     | rssi          | `i8`     | From RX ctrl                               |
| 19     | noise_floor   | `i8`     | From RX ctrl when available, else `0`      |
| 20     | channel       | `u8`     | Primary channel                            |
| 21     | mac           | `u8[6]`  | Transmitter MAC of the measured frame       |
| 27     | cycle_id      | `u32`    | Mesh cycle from SCHED                      |
| 31     | slot_idx      | `u8`     | Slot when sample was captured              |
| 32     | tx_node_id    | `u8`     | Logical TX node (`0` = Pi)                 |
| 33     | rx_node_id    | `u8`     | Logical RX node (this ESP)                 |
| 34     | probe_seq     | `u32`    | Probe seq if known, else `0`               |
| 38     | len           | `u16`    | Length of `iq[]` in bytes                  |
| 40     | iq            | `i8[len]`| CSI I/Q samples                            |

**Header size:** 40 bytes  
**Max `len`:** 512

NDJSON storage keys each sample by `(cycle_id, tx_node_id, rx_node_id)`.

### Batching

During a REPORT slot the ESP may send **one UDP datagram per buffered sample** (CSI2 frame each), or pack sequentially. The collector treats each CSI2 datagram independently. Firmware default: one CSI2 UDP datagram per sample, all sent in the assigned REPORT slot.

## I/Q encoding (classic ESP32 LLTF)

Espressif documents each subcarrier as two signed bytes:

1. Imaginary part (`i8`)
2. Real part (`i8`)

Amplitude for subcarrier `k`:

```
amp[k] = sqrt(imag[k]^2 + real[k]^2)
```

with `imag = iq[2*k]`, `real = iq[2*k + 1]`.

## Constants

```
CSI1_MAGIC   = 0x43534931
CSI1_VERSION = 1
CSI1_HDR_LEN = 29

CSI2_MAGIC   = 0x43534932
CSI2_VERSION = 2
CSI2_HDR_LEN = 40

CSI_MAX_IQ   = 512
```

## Ports (defaults)

| Role        | UDP port |
|-------------|----------|
| Probes (Pi ↔ ESP32) | 5005 |
| CSI uplink (ESP32 → Pi) | 5006 |
| SCHED control (Pi → ESP32) | 5007 |
