#!/usr/bin/env python3
"""Summarize per-link fill / RSSI / health for a CSI2 NDJSON run."""

from __future__ import annotations

import argparse
import json
import statistics as stats
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

EXPECTED_LINKS = [
    (0, 1),
    (0, 2),
    (0, 3),
    (1, 2),
    (1, 3),
    (2, 1),
    (2, 3),
    (3, 1),
    (3, 2),
]


def analyze(path: Path, expect: Iterable[Tuple[int, int]] = EXPECTED_LINKS) -> dict:
    rows = [json.loads(line) for line in path.open() if line.strip()]
    csi2 = [r for r in rows if r.get("magic") == "CSI2"]
    if not csi2:
        raise SystemExit(f"no CSI2 frames in {path}")

    expected = list(expect)
    cycles = sorted({r["cycle_id"] for r in csi2})
    per_cycle: Dict[int, set] = defaultdict(set)
    rssi: Dict[Tuple[int, int], List[float]] = defaultdict(list)
    amp: Dict[Tuple[int, int], List[float]] = defaultdict(list)
    nf: Dict[Tuple[int, int], List[float]] = defaultdict(list)
    rx_frames: Counter = Counter()

    for r in csi2:
        tx, rx = int(r["tx_node_id"]), int(r["rx_node_id"])
        if tx == rx:
            continue
        key = (tx, rx)
        per_cycle[r["cycle_id"]].add(key)
        rssi[key].append(float(r["rssi"]))
        amp[key].append(float(r["mean_amp"]))
        nf[key].append(float(r["noise_floor"]))
        rx_frames[rx] += 1

    n_cycles = len(cycles)
    uniq = [len(per_cycle[c]) for c in cycles]

    links = []
    for key in expected:
        vals = rssi.get(key, [])
        fill = sum(1 for c in cycles if key in per_cycle[c]) / n_cycles
        if not vals:
            links.append(
                {
                    "link": f"{key[0]}->{key[1]}",
                    "fill": 0.0,
                    "n": 0,
                    "rssi": None,
                    "snr": None,
                    "amp": None,
                    "score": 0.0,
                }
            )
            continue
        mean_rssi = stats.mean(vals)
        mean_nf = stats.mean(nf[key])
        mean_amp = stats.mean(amp[key])
        cv = (stats.pstdev(amp[key]) / mean_amp) if mean_amp else 1.0
        snr = mean_rssi - mean_nf
        snr_f = max(0.0, min(1.0, (snr - 15.0) / 30.0))
        stab = max(0.0, min(1.0, 1.0 - cv))
        score = round(0.55 * fill + 0.3 * snr_f + 0.15 * stab, 3)
        links.append(
            {
                "link": f"{key[0]}->{key[1]}",
                "fill": round(fill, 3),
                "n": len(vals),
                "rssi": round(mean_rssi, 1),
                "snr": round(snr, 1),
                "amp": round(mean_amp, 2),
                "score": score,
            }
        )

    return {
        "file": path.name,
        "n_cycles": n_cycles,
        "median_unique_links": stats.median(uniq),
        "pct_cycles_9links": round(sum(1 for u in uniq if u >= 9) / n_cycles, 3),
        "mean_score": round(stats.mean(x["score"] for x in links), 3),
        "rx_frames": dict(sorted(rx_frames.items())),
        "links": links,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ndjson", type=Path, help="CSI collector NDJSON path")
    parser.add_argument("--json", action="store_true", help="Print JSON instead of text")
    args = parser.parse_args()

    summary = analyze(args.ndjson)
    if args.json:
        print(json.dumps(summary, indent=2))
        return 0

    print(
        f"{summary['file']}: cycles={summary['n_cycles']} "
        f"med_uniq={summary['median_unique_links']} "
        f"pct9={summary['pct_cycles_9links']:.0%} "
        f"mean_score={summary['mean_score']:.3f}"
    )
    print(f"rx_frames={summary['rx_frames']}")
    print(f"{'link':8} {'fill':>7} {'n':>5} {'rssi':>7} {'snr':>6} {'score':>6}")
    for row in sorted(summary["links"], key=lambda x: -x["score"]):
        rssi = "—" if row["rssi"] is None else f"{row['rssi']:.1f}"
        snr = "—" if row["snr"] is None else f"{row['snr']:.1f}"
        print(
            f"{row['link']:8} {row['fill']:7.1%} {row['n']:5d} {rssi:>7} {snr:>6} {row['score']:6.3f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
