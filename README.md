# WiFi Sensing

Raspberry Pi runs a Wi-Fi AP and sends UDP probes. Classic ESP32 stations capture Channel State Information (CSI) on those packets and stream binary frames back to the Pi.

## Architecture

```
Pi (AP 192.168.4.1)  --UDP probes:5005-->  ESP32 STA
Pi collector:5006    <--CSI1 frames------  ESP32 STA
```

See [protocol/csi_frame.md](protocol/csi_frame.md) for the uplink wire format.

## Hardware

- Raspberry Pi with a working `wlan0` (or set `WIFI_IFACE`)
- Classic **ESP32** (not S2/S3/C3) with ESP-IDF toolchains
- 2.4 GHz only (channel 6 by default)

## Quick start



### 1. Configure

```bash
cp configs/sensing.env.example configs/sensing.env
# edit SSID / PSK / iface if needed
```

SSID and PSK in `configs/sensing.env` must match the ESP32 firmware defaults (or rebuild firmware after changing `[firmware/esp32_csi_node/sdkconfig.defaults](firmware/esp32_csi_node/sdkconfig.defaults)`).

### 2. Flash an ESP32 node

```bash
cd firmware/esp32_csi_node
idf.py set-target esp32
# optional: set unique node id
idf.py menuconfig   # WiFi Sensing CSI Node → Node ID
idf.py -p /dev/ttyUSB0 flash monitor
```

Wait until the serial log shows `got ip` and `CSI enabled`.

For a second board, set a different `CONFIG_CSI_NODE_ID` before flashing.

### 3. Bring up the Pi AP

On the Raspberry Pi (from the repo root):

```bash
sudo apt-get update
sudo apt-get install -y hostapd dnsmasq
sudo systemctl stop hostapd dnsmasq 2>/dev/null || true
sudo systemctl disable hostapd dnsmasq 2>/dev/null || true

set -a && source configs/sensing.env && set +a
sudo -E bash pi/ap/setup_ap.sh
```

Confirm leases appear under `/var/lib/misc/dnsmasq.leases` (or the path printed by the script) after the ESP32 connects.

### 4. Collect CSI and send probes

Terminal A (collector):

```bash
set -a && source configs/sensing.env && set +a
python3 pi/collector/collect_csi.py --port "${CSI_PORT}" --out-dir pi/runs
```

Terminal B (probes) — start **after** CSI is enabled on the ESP32:

```bash
set -a && source configs/sensing.env && set +a
python3 pi/probe/send_probes.py \
  --port "${PROBE_PORT}" \
  --rate "${PROBE_RATE_HZ}" \
  --settle "${PROBE_SETTLE_S}"
```

Or pin targets explicitly:

```bash
python3 pi/probe/send_probes.py --targets 192.168.4.10 --rate 30
```



### 5. Motion smoke test

1. Collector shows a non-zero pps and RSSI for your `node_id`.
2. Walk between the Pi and the ESP32.
3. Watch live `mean_amp` (and RSSI) shift in the collector output; inspect `pi/runs/<timestamp>.ndjson`.



## Success checklist

- [ ] ESP32 joins `wifi-sensing` and gets `192.168.4.x`
- [ ] Collector receives CSI1 frames at a steady rate while probes run
- [ ] Amplitude / RSSI changes with motion between Pi and ESP32
- [ ] (Optional) Two boards show distinct `node_id` values in one run file



## Tear down AP

```bash
sudo bash pi/ap/teardown_ap.sh
```


## Layout


| Path                       | Role                      |
| -------------------------- | ------------------------- |
| `firmware/esp32_csi_node/` | ESP-IDF CSI node          |
| `pi/ap/`                   | hostapd + dnsmasq hotspot |
| `pi/probe/`                | UDP probe sender          |
| `pi/collector/`            | CSI uplink logger         |
| `protocol/`                | CSI1 frame spec           |
| `configs/`                 | shared env defaults       |