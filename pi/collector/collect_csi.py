#!/usr/bin/env python3
"""Listen for CSI1/CSI2 UDP frames and write NDJSON run logs."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

# Allow running as script from repo root or collector/
sys.path.insert(0, str(Path(__file__).resolve().parent))
from decode import Csi2Frame, parse_csi_frame  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="Collect CSI1/CSI2 UDP frames from ESP32 nodes")
    parser.add_argument("--port", type=int, default=int(os.environ.get("CSI_PORT", "5006")))
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--out-dir", default="pi/runs")
    parser.add_argument("--raw", action="store_true", help="Also write raw binary sidecar")
    parser.add_argument("--stats-every", type=float, default=2.0, help="Seconds between live stats")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    ndjson_path = out_dir / f"{stamp}.ndjson"
    raw_path = out_dir / f"{stamp}.bin" if args.raw else None

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    sock.settimeout(0.5)

    print(f"listening on udp://{args.bind}:{args.port}")
    print(f"writing {ndjson_path}")
    if raw_path:
        print(f"raw dump {raw_path}")

    counts: dict[int, int] = defaultdict(int)
    link_counts: dict[str, int] = defaultdict(int)
    last_rssi: dict[int, int] = {}
    last_amp: dict[int, float] = {}
    last_cycle_links: dict[int, set[str]] = defaultdict(set)
    total = 0
    bad = 0
    t0 = time.monotonic()
    last_stats = t0

    ndjson_f = ndjson_path.open("a", encoding="utf-8")
    raw_f = raw_path.open("ab") if raw_path else None

    try:
        while True:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                data = None
                addr = None

            now = time.monotonic()
            if data is not None:
                frame = parse_csi_frame(data)
                if frame is None:
                    bad += 1
                else:
                    total += 1
                    counts[frame.node_id] += 1
                    last_rssi[frame.node_id] = frame.rssi
                    mean_amp = frame.mean_amplitude()
                    last_amp[frame.node_id] = mean_amp

                    record = frame.to_ndjson_dict()
                    record["src_ip"] = addr[0] if addr else None
                    record["host_recv_ts"] = time.time()

                    if isinstance(frame, Csi2Frame):
                        link = f"{frame.tx_node_id}->{frame.rx_node_id}"
                        link_counts[link] += 1
                        last_cycle_links[frame.cycle_id].add(link)
                        # Keep only recent cycles in the rolling set
                        if len(last_cycle_links) > 20:
                            oldest = min(last_cycle_links)
                            del last_cycle_links[oldest]

                    ndjson_f.write(json.dumps(record, separators=(",", ":")) + "\n")
                    ndjson_f.flush()

                    if raw_f is not None:
                        raw_f.write(data)
                        raw_f.flush()

            if now - last_stats >= args.stats_every:
                elapsed = max(now - t0, 1e-6)
                parts = []
                for nid in sorted(counts):
                    pps = counts[nid] / elapsed
                    parts.append(
                        f"node={nid} n={counts[nid]} ~{pps:.1f}pps "
                        f"rssi={last_rssi.get(nid, '?')} mean_amp={last_amp.get(nid, 0):.2f}"
                    )
                summary = " | ".join(parts) if parts else "(no frames yet)"
                link_summary = ""
                if link_counts:
                    top = sorted(link_counts.items(), key=lambda kv: kv[0])
                    link_summary = " links=" + ",".join(f"{k}:{v}" for k, v in top)
                    if last_cycle_links:
                        cyc = max(last_cycle_links)
                        nlinks = len(last_cycle_links[cyc])
                        link_summary += f" cycle={cyc} unique_links={nlinks}"
                print(f"[{elapsed:.0f}s] total={total} bad={bad}  {summary}{link_summary}")
                last_stats = now
                if elapsed >= 30:
                    counts.clear()
                    link_counts.clear()
                    t0 = now
                    total = 0
                    bad = 0
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        ndjson_f.close()
        if raw_f:
            raw_f.close()
        sock.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
