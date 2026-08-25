#!/usr/bin/env python3
"""Send UDP probes to ESP32 CSI nodes to stimulate CSI capture.

Start this AFTER the ESP32 logs "CSI enabled" / "ready — start Pi probes".
Classic ESP32 can miss CSI if intense traffic begins before CSI is enabled.
"""

from __future__ import annotations

import argparse
import os
import socket
import struct
import time
from pathlib import Path
from typing import List, Optional


DEFAULT_LEASE_CANDIDATES = [
    Path("pi/ap/.runtime/dnsmasq.leases"),
    Path("/var/lib/misc/dnsmasq.leases"),
    Path("/var/lib/dnsmasq/dnsmasq.leases"),
]


def parse_leases(path: Path) -> List[str]:
    """dnsmasq lease line: <expiry> <mac> <ip> <hostname> <clientid>"""
    ips: List[str] = []
    if not path.is_file():
        return ips
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        ip = parts[2]
        if ip.count(".") == 3 and ip.startswith("192.168.4."):
            if ip not in ips:
                ips.append(ip)
    return ips


def discover_targets(explicit: List[str], lease_file: Optional[str]) -> List[str]:
    if explicit:
        return explicit
    candidates = []
    if lease_file:
        candidates.append(Path(lease_file))
    candidates.extend(DEFAULT_LEASE_CANDIDATES)
    for path in candidates:
        ips = parse_leases(path)
        if ips:
            print(f"discovered {len(ips)} lease(s) from {path}: {', '.join(ips)}")
            return ips
    raise SystemExit(
        "No targets found. Pass --targets 192.168.4.10 or ensure dnsmasq leases exist."
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="UDP probe sender for CSI sensing")
    parser.add_argument("--port", type=int, default=int(os.environ.get("PROBE_PORT", "5005")))
    parser.add_argument("--rate", type=float, default=float(os.environ.get("PROBE_RATE_HZ", "30")))
    parser.add_argument(
        "--settle",
        type=float,
        default=float(os.environ.get("PROBE_SETTLE_S", "2.0")),
        help="Seconds to wait before sending (after CSI enable)",
    )
    parser.add_argument("--targets", nargs="*", default=[], help="ESP32 IPs")
    parser.add_argument("--lease-file", default=os.environ.get("DNSMASQ_LEASES", ""))
    parser.add_argument("--payload-size", type=int, default=32, help="UDP payload bytes (>= 12)")
    args = parser.parse_args()

    if args.rate <= 0:
        raise SystemExit("--rate must be > 0")
    if args.payload_size < 12:
        raise SystemExit("--payload-size must be >= 12")

    targets = discover_targets(args.targets, args.lease_file or None)
    interval = 1.0 / args.rate

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"settle {args.settle:.1f}s then probe {targets} @ {args.rate:.1f} Hz udp/{args.port}")
    time.sleep(args.settle)

    seq = 0
    sent = 0
    t0 = time.monotonic()
    next_t = t0
    try:
        while True:
            now = time.monotonic()
            if now < next_t:
                time.sleep(min(next_t - now, 0.005))
                continue
            next_t += interval
            # Avoid backlog if we fell behind
            if next_t < time.monotonic() - interval:
                next_t = time.monotonic()

            seq += 1
            # counter(u32) + host monotonic us(u64) + pad
            host_us = int(time.monotonic() * 1_000_000) & 0xFFFFFFFFFFFFFFFF
            payload = struct.pack("<IQ", seq & 0xFFFFFFFF, host_us)
            payload = payload + (b"\x00" * (args.payload_size - len(payload)))

            for ip in targets:
                sock.sendto(payload, (ip, args.port))
                sent += 1

            elapsed = time.monotonic() - t0
            if seq % max(1, int(args.rate * 5)) == 0:
                pps = sent / max(elapsed, 1e-6)
                print(f"seq={seq} sent={sent} ~{pps:.1f} pkt/s -> {targets}")
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
