#!/usr/bin/env bash
# Bring up a Wi-Fi AP for CSI sensing using hostapd + dnsmasq.
# Run as root with env from configs/sensing.env (see README).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUNTIME_DIR="${SCRIPT_DIR}/.runtime"
mkdir -p "${RUNTIME_DIR}"

WIFI_IFACE="${WIFI_IFACE:-wlan0}"
WIFI_SSID="${WIFI_SSID:-wifi-sensing}"
WIFI_PSK="${WIFI_PSK:-wifisense123}"
WIFI_CHANNEL="${WIFI_CHANNEL:-6}"
AP_IP="${AP_IP:-192.168.4.1}"
AP_NETMASK="${AP_NETMASK:-255.255.255.0}"
DHCP_RANGE_START="${DHCP_RANGE_START:-192.168.4.10}"
DHCP_RANGE_END="${DHCP_RANGE_END:-192.168.4.50}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo -E bash pi/ap/setup_ap.sh" >&2
  exit 1
fi

if [[ "${#WIFI_PSK}" -lt 8 ]]; then
  echo "WIFI_PSK must be at least 8 characters" >&2
  exit 1
fi

if ! ip link show "${WIFI_IFACE}" >/dev/null 2>&1; then
  echo "Interface ${WIFI_IFACE} not found" >&2
  exit 1
fi

command -v hostapd >/dev/null || { echo "hostapd not installed" >&2; exit 1; }
command -v dnsmasq >/dev/null || { echo "dnsmasq not installed" >&2; exit 1; }

# Stop NetworkManager / wpa_supplicant control of the iface when present.
if command -v nmcli >/dev/null 2>&1; then
  nmcli dev set "${WIFI_IFACE}" managed no || true
fi
systemctl stop wpa_supplicant 2>/dev/null || true
killall wpa_supplicant 2>/dev/null || true

HOSTAPD_CONF="${RUNTIME_DIR}/hostapd.conf"
DNSMASQ_CONF="${RUNTIME_DIR}/dnsmasq.conf"
LEASES_FILE="${RUNTIME_DIR}/dnsmasq.leases"
PID_HOSTAPD="${RUNTIME_DIR}/hostapd.pid"
PID_DNSMASQ="${RUNTIME_DIR}/dnsmasq.pid"

cat >"${HOSTAPD_CONF}" <<EOF
interface=${WIFI_IFACE}
driver=nl80211
ssid=${WIFI_SSID}
hw_mode=g
channel=${WIFI_CHANNEL}
ieee80211n=1
wmm_enabled=1
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=${WIFI_PSK}
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
country_code=US
EOF

cat >"${DNSMASQ_CONF}" <<EOF
interface=${WIFI_IFACE}
bind-interfaces
dhcp-range=${DHCP_RANGE_START},${DHCP_RANGE_END},12h
dhcp-option=3,${AP_IP}
dhcp-option=6,${AP_IP}
dhcp-leasefile=${LEASES_FILE}
log-dhcp
EOF

ip link set "${WIFI_IFACE}" down
ip addr flush dev "${WIFI_IFACE}"
ip addr add "${AP_IP}/24" dev "${WIFI_IFACE}"
ip link set "${WIFI_IFACE}" up

# Optional: enable IPv4 forwarding (not required for local CSI LAN)
# sysctl -w net.ipv4.ip_forward=1 >/dev/null

pkill -F "${PID_HOSTAPD}" 2>/dev/null || true
pkill -F "${PID_DNSMASQ}" 2>/dev/null || true
rm -f "${PID_HOSTAPD}" "${PID_DNSMASQ}"
: >"${LEASES_FILE}"

hostapd -B -P "${PID_HOSTAPD}" "${HOSTAPD_CONF}"
dnsmasq -C "${DNSMASQ_CONF}" -x "${PID_DNSMASQ}"

echo "AP up on ${WIFI_IFACE}"
echo "  SSID:    ${WIFI_SSID}"
echo "  Channel: ${WIFI_CHANNEL}"
echo "  Gateway: ${AP_IP}"
echo "  Leases:  ${LEASES_FILE}"
echo "  hostapd: ${HOSTAPD_CONF}"
echo
echo "Waiting for ESP32 DHCP leases..."
sleep 1
if [[ -s "${LEASES_FILE}" ]]; then
  echo "Current leases:"
  cat "${LEASES_FILE}"
else
  echo "(none yet — power on / flash ESP32, then re-check ${LEASES_FILE})"
fi
