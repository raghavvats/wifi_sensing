# CSI1 binary frame

UDP payload from each ESP32 CSI node to the Raspberry Pi collector. All multi-byte integers are **little-endian**.

## Layout

| Offset | Field         | Type     | Notes                                      |
|--------|---------------|----------|--------------------------------------------|
| 0      | magic         | `u32`    | `0x43534931` (`CSI1` in ASCII)             |
| 4      | version       | `u8`     | `1`                                        |
| 5      | node_id       | `u8`     | Node identity (compile-time / NVS)         |
| 6      | seq           | `u32`    | Monotonic CSI frame counter                |
| 10     | t_us          | `u64`    | ESP `esp_timer_get_time()` microseconds    |
| 18     | rssi          | `i8`     | From `wifi_pkt_rx_ctrl_t.rssi`             |
| 19     | noise_floor   | `i8`     | From RX ctrl when available, else `0`      |
| 20     | channel       | `u8`     | Primary channel                            |
| 21     | mac           | `u8[6]`  | Transmitter MAC (probe source / AP STA)    |
| 27     | len           | `u16`    | Length of `iq[]` in bytes                  |
| 29     | iq            | `i8[len]`| CSI I/Q samples                            |

**Header size:** 29 bytes  
**Max `len`:** 512 (firmware truncates if larger)

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
```

## Ports (defaults)

| Role        | UDP port |
|-------------|----------|
| Probes (Pi → ESP32) | 5005 |
| CSI uplink (ESP32 → Pi) | 5006 |
