#!/usr/bin/env bash
# Tear down the CSI sensing AP started by setup_ap.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_DIR="${SCRIPT_DIR}/.runtime"
WIFI_IFACE="${WIFI_IFACE:-wlan0}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo -E bash pi/ap/teardown_ap.sh" >&2
  exit 1
fi

if [[ -f "${RUNTIME_DIR}/hostapd.pid" ]]; then
  pkill -F "${RUNTIME_DIR}/hostapd.pid" 2>/dev/null || true
  rm -f "${RUNTIME_DIR}/hostapd.pid"
fi
if [[ -f "${RUNTIME_DIR}/dnsmasq.pid" ]]; then
  pkill -F "${RUNTIME_DIR}/dnsmasq.pid" 2>/dev/null || true
  rm -f "${RUNTIME_DIR}/dnsmasq.pid"
fi

killall hostapd 2>/dev/null || true
killall dnsmasq 2>/dev/null || true

if ip link show "${WIFI_IFACE}" >/dev/null 2>&1; then
  ip addr flush dev "${WIFI_IFACE}" || true
  ip link set "${WIFI_IFACE}" down || true
fi

if command -v nmcli >/dev/null 2>&1; then
  nmcli dev set "${WIFI_IFACE}" managed yes || true
fi

echo "AP torn down on ${WIFI_IFACE}"
