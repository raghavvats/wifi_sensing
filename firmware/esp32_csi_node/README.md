# ESP32 CSI Node Firmware

Classic ESP32 firmware that joins the Pi AP, enables Wi-Fi CSI, and streams `CSI1` UDP frames to the collector.

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
