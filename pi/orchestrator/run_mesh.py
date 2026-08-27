#!/usr/bin/env python3
"""Mesh TDMA orchestrator: Pi probes + SCHED control for ESP CSI nodes.

Discovers peers via HELLO announces (firmware node_id + MAC + IP) so SYNC
matches CONFIG_CSI_NODE_ID. Falls back to --peers / leases + --id-map.
"""

from __future__ import annotations

import argparse
import os
import socket
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

SCHED_MAGIC = 0x53434844
SCHED_VERSION = 1
SCHED_TYPE_SLOT_BEGIN = 1
SCHED_TYPE_SYNC = 2
SCHED_TYPE_HELLO = 3
SCHED_HELLO_FMT = "<IBBB6s4sB"
SCHED_HELLO_LEN = struct.calcsize(SCHED_HELLO_FMT)  # 18
PHASE_PROBE = 0
PHASE_REPORT = 1
NODE_ID_PI = 0

DEFAULT_LEASE_CANDIDATES = [
    Path("pi/ap/.runtime/dnsmasq.leases"),
    Path("/var/lib/misc/dnsmasq.leases"),
    Path("/var/lib/dnsmasq/dnsmasq.leases"),
]


@dataclass
class Peer:
    node_id: int
    ip: str
    mac: bytes  # 6 bytes


def parse_mac(s: str) -> bytes:
    parts = s.replace("-", ":").lower().split(":")
    if len(parts) != 6:
        raise ValueError(f"bad mac: {s}")
    return bytes(int(p, 16) for p in parts)


def mac_str(mac: bytes) -> str:
    return ":".join(f"{b:02x}" for b in mac)


def parse_leases(path: Path) -> List[Tuple[str, str]]:
    out: List[Tuple[str, str]] = []
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        mac, ip = parts[1], parts[2]
        if ip.count(".") == 3 and ip.startswith("192.168.4."):
            out.append((mac, ip))
    return out


def discover_lease_peers(lease_file: Optional[str]) -> List[Tuple[str, str]]:
    candidates: List[Path] = []
    if lease_file:
        candidates.append(Path(lease_file))
    candidates.extend(DEFAULT_LEASE_CANDIDATES)
    for path in candidates:
        rows = parse_leases(path)
        if rows:
            print(f"discovered {len(rows)} lease(s) from {path}")
            return rows
    return []


def parse_hello(data: bytes) -> Optional[Peer]:
    if len(data) < SCHED_HELLO_LEN:
        return None
    magic, version, typ, node_id, mac, ipv4, _pad = struct.unpack(
        SCHED_HELLO_FMT, data[:SCHED_HELLO_LEN]
    )
    if magic != SCHED_MAGIC or version != SCHED_VERSION or typ != SCHED_TYPE_HELLO:
        return None
    if node_id == 0:
        return None
    ip = socket.inet_ntoa(ipv4)
    if ip == "0.0.0.0":
        return None
    return Peer(node_id=node_id, ip=ip, mac=mac)


def discover_hello_peers(
    hello_port: int,
    wait_s: float,
    expect: int,
) -> List[Peer]:
    """Listen for HELLO announces; return unique peers keyed by firmware node_id."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", hello_port))
    sock.settimeout(0.2)
    print(f"waiting up to {wait_s:.1f}s for HELLO on :{hello_port} (expect>={expect})")

    by_id: Dict[int, Peer] = {}
    t_end = time.monotonic() + wait_s
    try:
        while time.monotonic() < t_end:
            if expect > 0 and len(by_id) >= expect:
                # brief grace to catch stragglers
                if time.monotonic() + 0.5 >= t_end:
                    break
                t_end = min(t_end, time.monotonic() + 0.5)
            try:
                data, addr = sock.recvfrom(64)
            except socket.timeout:
                continue
            peer = parse_hello(data)
            if peer is None:
                continue
            # Prefer IP from packet; fall back to UDP source if needed
            if peer.ip == "0.0.0.0":
                peer = Peer(node_id=peer.node_id, ip=addr[0], mac=peer.mac)
            prev = by_id.get(peer.node_id)
            if prev is None or prev.mac != peer.mac or prev.ip != peer.ip:
                print(
                    f"  HELLO node_id={peer.node_id} ip={peer.ip} mac={mac_str(peer.mac)}"
                )
            by_id[peer.node_id] = peer
    finally:
        sock.close()

    peers = sorted(by_id.values(), key=lambda p: p.node_id)
    print(f"HELLO discovery found {len(peers)} peer(s)")
    return peers


def build_peers_explicit(explicit: List[str]) -> List[Peer]:
    peers: List[Peer] = []
    for entry in explicit:
        parts = entry.split(":")
        if len(parts) < 8:
            raise SystemExit(f"bad --peers entry (want id:ip:mac): {entry}")
        node_id = int(parts[0])
        ip = parts[1]
        mac = parse_mac(":".join(parts[2:8]))
        peers.append(Peer(node_id=node_id, ip=ip, mac=mac))
    return peers


def build_peers_leases(lease_file: Optional[str], id_map: Dict[str, int]) -> List[Peer]:
    rows = discover_lease_peers(lease_file)
    if not rows:
        return []
    if not id_map:
        raise SystemExit(
            "Lease-only discovery cannot know firmware node_id. "
            "Wait for HELLO, or pass --peers / --id-map ip=node_id."
        )
    peers: List[Peer] = []
    used: set[int] = set()
    for mac_str_raw, ip in rows:
        if ip not in id_map:
            print(f"warn: lease {ip} has no --id-map entry; skipping")
            continue
        nid = id_map[ip]
        if nid in used:
            raise SystemExit(f"duplicate node_id {nid} in --id-map")
        used.add(nid)
        peers.append(Peer(node_id=nid, ip=ip, mac=parse_mac(mac_str_raw)))
    return peers


def pack_sync(probe_ms: int, report_ms: int, peers: List[Peer]) -> bytes:
    hdr = struct.pack(
        "<IBBHHBB",
        SCHED_MAGIC,
        SCHED_VERSION,
        SCHED_TYPE_SYNC,
        probe_ms & 0xFFFF,
        report_ms & 0xFFFF,
        len(peers) & 0xFF,
        0,
    )
    body = bytearray()
    for p in peers:
        ip_bytes = socket.inet_aton(p.ip)
        body += struct.pack("<B6s4sB", p.node_id & 0xFF, p.mac, ip_bytes, 0)
    return hdr + bytes(body)


def pack_slot_begin(
    cycle_id: int,
    slot_idx: int,
    tx_node: int,
    phase: int,
    rx_target: int = 0,
) -> bytes:
    return struct.pack(
        "<IBBIBBBB",
        SCHED_MAGIC,
        SCHED_VERSION,
        SCHED_TYPE_SLOT_BEGIN,
        cycle_id & 0xFFFFFFFF,
        slot_idx & 0xFF,
        tx_node & 0xFF,
        phase & 0xFF,
        rx_target & 0xFF,
    )


def slot_plan(peers: List[Peer]) -> List[Tuple[int, int, int, Optional[Peer]]]:
    plan: List[Tuple[int, int, int, Optional[Peer]]] = []
    slot = 0
    for p in peers:
        plan.append((slot, NODE_ID_PI, PHASE_PROBE, p))
        slot += 1
    for p in peers:
        plan.append((slot, p.node_id, PHASE_PROBE, None))
        slot += 1
    for p in peers:
        plan.append((slot, p.node_id, PHASE_REPORT, None))
        slot += 1
    return plan


def broadcast_sched(sock: socket.socket, port: int, peers: List[Peer], payload: bytes) -> None:
    for p in peers:
        sock.sendto(payload, (p.ip, port))


def main() -> int:
    parser = argparse.ArgumentParser(description="Mesh TDMA CSI orchestrator")
    parser.add_argument("--probe-port", type=int, default=int(os.environ.get("PROBE_PORT", "5005")))
    parser.add_argument(
        "--control-port",
        type=int,
        default=int(os.environ.get("MESH_CONTROL_PORT", "5007")),
    )
    parser.add_argument(
        "--hello-port",
        type=int,
        default=int(os.environ.get("MESH_HELLO_PORT", "5008")),
    )
    parser.add_argument(
        "--hello-wait",
        type=float,
        default=float(os.environ.get("MESH_HELLO_WAIT_S", "8")),
        help="Seconds to collect HELLO announces before SYNC",
    )
    parser.add_argument(
        "--expect-peers",
        type=int,
        default=int(os.environ.get("MESH_EXPECT_PEERS", "3")),
        help="Stop HELLO wait early once this many unique node_ids are seen",
    )
    parser.add_argument(
        "--probe-slot-ms",
        type=float,
        default=float(os.environ.get("MESH_PROBE_SLOT_MS", "20")),
    )
    parser.add_argument(
        "--report-slot-ms",
        type=float,
        default=float(os.environ.get("MESH_REPORT_SLOT_MS", "40")),
    )
    parser.add_argument(
        "--settle",
        type=float,
        default=float(os.environ.get("PROBE_SETTLE_S", "1.0")),
    )
    parser.add_argument(
        "--cycles",
        type=int,
        default=int(os.environ.get("MESH_CYCLES", "0")),
        help="0 = run forever",
    )
    parser.add_argument("--lease-file", default=os.environ.get("DNSMASQ_LEASES", ""))
    parser.add_argument(
        "--peers",
        nargs="*",
        default=[],
        help="Explicit peers: node_id:ip:aa:bb:cc:dd:ee:ff (skips HELLO)",
    )
    parser.add_argument(
        "--id-map",
        nargs="*",
        default=[],
        help="Lease fallback only: ip=node_id",
    )
    parser.add_argument("--payload-size", type=int, default=32)
    args = parser.parse_args()

    if args.probe_slot_ms <= 0 or args.report_slot_ms <= 0:
        raise SystemExit("slot durations must be > 0")
    if args.payload_size < 12:
        raise SystemExit("--payload-size must be >= 12")

    id_map: Dict[str, int] = {}
    for item in args.id_map:
        ip, _, nid = item.partition("=")
        if not ip or not nid:
            raise SystemExit(f"bad --id-map entry: {item}")
        id_map[ip] = int(nid)

    if args.settle > 0:
        print(f"settle {args.settle:.1f}s")
        time.sleep(args.settle)

    if args.peers:
        peers = build_peers_explicit(args.peers)
    else:
        peers = discover_hello_peers(args.hello_port, args.hello_wait, args.expect_peers)
        if not peers:
            peers = build_peers_leases(args.lease_file or None, id_map)

    peers.sort(key=lambda p: p.node_id)
    if not peers:
        raise SystemExit(
            "No peers. Flash mesh firmware (HELLO), wait for ESPs, or pass --peers."
        )
    if len(peers) > 8:
        raise SystemExit("at most 8 peers supported")

    # Detect duplicate node_ids
    ids = [p.node_id for p in peers]
    if len(ids) != len(set(ids)):
        raise SystemExit(f"duplicate node_id in peer list: {ids}")

    plan = slot_plan(peers)

    print("peers (SYNC will use these firmware IDs):")
    for p in peers:
        print(f"  node_id={p.node_id} ip={p.ip} mac={mac_str(p.mac)}")
    print(
        f"slots={len(plan)} probe_ms={args.probe_slot_ms} report_ms={args.report_slot_ms} "
        f"control=:{args.control_port} probe=:{args.probe_port}"
    )

    sched_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    discard_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    discard_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        discard_sock.bind(("0.0.0.0", args.probe_port))
        discard_sock.setblocking(False)
        print(f"discarding inbound probes on :{args.probe_port}")
    except OSError as e:
        print(f"warn: could not bind probe discard :{args.probe_port} ({e})")
        discard_sock.close()
        discard_sock = None

    sync = pack_sync(int(args.probe_slot_ms), int(args.report_slot_ms), peers)
    broadcast_sched(sched_sock, args.control_port, peers, sync)
    print("sent SYNC")
    time.sleep(0.05)

    probe_seq = 0
    cycle = 0
    t0 = time.monotonic()

    try:
        while True:
            cycle += 1
            for slot_idx, tx_node, phase, target in plan:
                if discard_sock is not None:
                    try:
                        while True:
                            discard_sock.recvfrom(256)
                    except BlockingIOError:
                        pass

                rx_target = target.node_id if target is not None else 0
                msg = pack_slot_begin(cycle, slot_idx, tx_node, phase, rx_target)
                broadcast_sched(sched_sock, args.control_port, peers, msg)

                if phase == PHASE_PROBE and tx_node == NODE_ID_PI and target is not None:
                    probe_seq += 1
                    host_us = int(time.monotonic() * 1_000_000) & 0xFFFFFFFFFFFFFFFF
                    payload = struct.pack("<IQ", probe_seq & 0xFFFFFFFF, host_us)
                    payload = payload + (b"\x00" * (args.payload_size - len(payload)))
                    probe_sock.sendto(payload, (target.ip, args.probe_port))

                slot_s = (
                    args.probe_slot_ms / 1000.0
                    if phase == PHASE_PROBE
                    else args.report_slot_ms / 1000.0
                )
                time.sleep(slot_s)

            if cycle % 10 == 0:
                elapsed = time.monotonic() - t0
                hz = cycle / max(elapsed, 1e-6)
                print(f"cycle={cycle} ~{hz:.2f} mesh_Hz peers={len(peers)}")

            if args.cycles > 0 and cycle >= args.cycles:
                print(f"completed {cycle} cycles")
                break
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        sched_sock.close()
        probe_sock.close()
        if discard_sock is not None:
            discard_sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
