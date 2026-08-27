# ESP32 CSI Node Firmware

Classic ESP32 firmware that joins the Pi AP, follows mesh TDMA SCHED packets, captures CSI (Pi probes + peer overhear), and streams `CSI2` UDP frames to the collector during REPORT slots.

Legacy star mode: set `CONFIG_CSI_MESH_TDMA=n` for immediate `CSI1` uplink with `pi/probe/send_probes.py`.

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) v5.x
- Classic ESP32 board (CSI enabled in sdkconfig)

## Build & flash

```bash
. $HOME/esp/esp-idf/export.sh   # or your IDF install
cd firmware/esp32_csi_node
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Unique node IDs

```bash
idf.py menuconfig
# WiFi Sensing CSI Node → Node ID
```

Or append to `sdkconfig.defaults` before build:

```
CONFIG_CSI_NODE_ID=2
```

SSID/password defaults must match `configs/sensing.env` on the Pi.

## Mesh behavior

- Announces HELLO (`node_id` + MAC + IP) to Pi `:5008` about once per second
- Listens for SCHED on UDP `:5007` (`SYNC` + `SLOT_BEGIN`)
- Whitelists AP BSSID + peer STA MACs from `SYNC`
- Keeps **one** CSI sample per PROBE slot, only from the **scheduled TX MAC**
- Unicasts a probe to the Pi when scheduled as TX
- Flushes CSI2 frames only in its REPORT slot
