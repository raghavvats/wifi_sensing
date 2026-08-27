# WiFi Sensing

Raspberry Pi runs a Wi-Fi AP, orchestrates TDMA probe slots, and collects CSI. Classic ESP32 stations measure Channel State Information on Pi and peer-overheard probes, then stream binary frames back to the Pi.

## Architecture

### Star (legacy)

```
Pi (AP 192.168.4.1)  --UDP probes:5005-->  ESP32 STA
Pi collector:5006    <--CSI1 frames------  ESP32 STA
```

### Mesh TDMA (default firmware)

```
Pi orchestrator :5007 SCHED + :5005 Pi probes
ESP A/B/C       --overhear peer uplinks--> CSI buffer
ESP report slot <--CSI2 frames-----------> Pi collector :5006
```

See [protocol/csi_frame.md](protocol/csi_frame.md) and [protocol/sched.md](protocol/sched.md).

## Hardware

- Raspberry Pi with a working `wlan0` (or set `WIFI_IFACE`)
- Classic **ESP32** (not S2/S3/C3) with ESP-IDF toolchains
- 2.4 GHz only (channel 6 by default)
- For mesh: **3 ESP32 boards** with distinct `CONFIG_CSI_NODE_ID` (e.g. 1, 2, 3)



## Quick start



### 1. Configure

```bash
cp configs/sensing.env.example configs/sensing.env
# edit SSID / PSK / iface if needed
```

SSID and PSK in `configs/sensing.env` must match the ESP32 firmware defaults (or rebuild firmware after changing `[firmware/esp32_csi_node/sdkconfig.defaults](firmware/esp32_csi_node/sdkconfig.defaults)`).

### 2. Flash ESP32 nodes

```bash
cd firmware/esp32_csi_node
idf.py set-target esp32
idf.py menuconfig   # WiFi Sensing CSI Node → Node ID (unique per board)
idf.py -p /dev/ttyUSB0 flash monitor
```

Wait until the serial log shows `got ip`, `CSI enabled`, and `mesh TDMA enabled`.

### 3. Bring up the Pi AP

```bash
sudo apt-get update
sudo apt-get install -y hostapd dnsmasq
sudo systemctl stop hostapd dnsmasq 2>/dev/null || true
sudo systemctl disable hostapd dnsmasq 2>/dev/null || true

set -a && source configs/sensing.env && set +a
sudo -E bash pi/ap/setup_ap.sh
```

Confirm leases appear after each ESP connects.

### 4. Mesh TDMA collect + orchestrate

Power on all ESPs first and wait until each logs `ready` (they announce HELLO every ~1s).

Terminal A (collector):

```bash
set -a && source configs/sensing.env && set +a
python3 pi/collector/collect_csi.py --port "${CSI_PORT}" --out-dir pi/runs
```

Terminal B (orchestrator) — discovers peers from HELLO (`node_id` + MAC + IP):

```bash
set -a && source configs/sensing.env && set +a
python3 pi/orchestrator/run_mesh.py
```

You should see lines like `HELLO node_id=1 ip=... mac=...` then `sent SYNC` with matching firmware IDs.

Optional overrides if HELLO is unavailable:

```bash
python3 pi/orchestrator/run_mesh.py \
  --peers \
    1:192.168.4.10:aa:bb:cc:dd:ee:01 \
    2:192.168.4.11:aa:bb:cc:dd:ee:02 \
    3:192.168.4.12:aa:bb:cc:dd:ee:03
```



### 5. Star topology fallback

```bash
python3 pi/probe/send_probes.py --port "${PROBE_PORT}" --rate "${PROBE_RATE_HZ}"
```

Requires firmware built with `CONFIG_CSI_MESH_TDMA=n` for immediate CSI1 uplink.

### 6. Motion smoke test

1. Collector shows CSI2 frames and `unique_links` approaching 9 per cycle (3 Pi→ESP + 6 ESP↔ESP).
2. Walk the quadrilateral; watch `mean_amp` / RSSI shift per `tx->rx` link in `pi/runs/<timestamp>.ndjson`.



## Mesh bring-up validation

1. **HELLO → SYNC IDs match firmware** — Orchestrator peer list `node_id` equals each board’s `CONFIG_CSI_NODE_ID`; ESP serial shows the same MACs under `SYNC peers=`.
2. **Overhear MACs** — During ESP TX slots, peer CSI2 records show `mac` = transmitting STA (not Pi BSSID). No bogus `tx==rx` self-links.
3. **One sample per link slot** — Per cycle, about one CSI2 record per directed link (not dozens of `0→N` floods).
4. **Quiet probe phase** — CSI uplink (`:5006`) only in REPORT windows.
5. **Nine link keys** — For a given `cycle_id`, NDJSON should contain nine distinct `(tx_node_id, rx_node_id)` pairs for a 3-ESP mesh.
6. **Tune slots** — If peer overhear is sparse, increase `MESH_PROBE_SLOT_MS` (default 20). If the highest `node_id` under-reports, increase `MESH_REPORT_SLOT_MS` (default 40) and ensure firmware flush is synchronous (current tree).
7. **Geometry** — Spread Pi + 3 ESPs toward room corners / a rectangle. Target RSSI roughly **-45 to -65 dBm** on every directed link; avoid clustering (e.g. one ESP next to the Pi at −20 dBm while another pair sits together at −75 dBm peer RSSI).



## Success checklist

- [ ] Each ESP32 joins `wifi-sensing` and gets `192.168.4.x`
- [ ] Orchestrator prints peers and advancing `cycle=`
- [ ] Collector receives CSI2 frames with `cycle_id` / `tx_node_id` / `rx_node_id`
- [ ] ~9 unique links per cycle with 3 ESPs
- [ ] Amplitude / RSSI changes with motion on multiple links



## Tear down AP

```bash
sudo bash pi/ap/teardown_ap.sh
```



## Layout


| Path                       | Role                                      |
| -------------------------- | ----------------------------------------- |
| `firmware/esp32_csi_node/` | ESP-IDF CSI node (mesh TDMA follower)     |
| `pi/ap/`                   | hostapd + dnsmasq hotspot                 |
| `pi/orchestrator/`         | Mesh TDMA SCHED + Pi probes               |
| `pi/probe/`                | Star-topology UDP probe sender (fallback) |
| `pi/collector/`            | CSI1/CSI2 uplink logger                   |
| `protocol/`                | CSI + SCHED wire specs                    |
| `configs/`                 | shared env defaults                       |


