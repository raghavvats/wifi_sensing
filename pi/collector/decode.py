#!/usr/bin/env python3
"""Decode CSI1 binary UDP frames (see protocol/csi_frame.md)."""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass
from typing import List, Optional, Tuple

CSI1_MAGIC = 0x43534931
CSI1_VERSION = 1
CSI1_HDR_FMT = "<IBBIQbbB6sH"  # little-endian header without iq[]
CSI1_HDR_LEN = struct.calcsize(CSI1_HDR_FMT)  # 29


@dataclass
class Csi1Frame:
    magic: int
    version: int
    node_id: int
    seq: int
    t_us: int
    rssi: int
    noise_floor: int
    channel: int
    mac: bytes
    len: int
    iq: bytes

    @property
    def mac_str(self) -> str:
        return ":".join(f"{b:02x}" for b in self.mac)

    def amplitudes(self) -> List[float]:
        out: List[float] = []
        n = len(self.iq) // 2
        for i in range(n):
            imag = int.from_bytes(self.iq[2 * i : 2 * i + 1], "little", signed=True)
            real = int.from_bytes(self.iq[2 * i + 1 : 2 * i + 2], "little", signed=True)
            out.append(math.sqrt(imag * imag + real * real))
        return out

    def mean_amplitude(self) -> float:
        amps = self.amplitudes()
        if not amps:
            return 0.0
        return sum(amps) / len(amps)

    def to_ndjson_dict(self) -> dict:
        amps = self.amplitudes()
        return {
            "magic": "CSI1",
            "version": self.version,
            "node_id": self.node_id,
            "seq": self.seq,
            "t_us": self.t_us,
            "rssi": self.rssi,
            "noise_floor": self.noise_floor,
            "channel": self.channel,
            "mac": self.mac_str,
            "len": self.len,
            "n_subcarriers": len(amps),
            "mean_amp": round(self.mean_amplitude(), 4),
            "amp": [round(a, 4) for a in amps],
        }


def parse_csi1(data: bytes) -> Optional[Csi1Frame]:
    if len(data) < CSI1_HDR_LEN:
        return None
    magic, version, node_id, seq, t_us, rssi, noise_floor, channel, mac, length = struct.unpack(
        CSI1_HDR_FMT, data[:CSI1_HDR_LEN]
    )
    if magic != CSI1_MAGIC or version != CSI1_VERSION:
        return None
    if length < 0 or CSI1_HDR_LEN + length > len(data):
        return None
    iq = data[CSI1_HDR_LEN : CSI1_HDR_LEN + length]
    return Csi1Frame(
        magic=magic,
        version=version,
        node_id=node_id,
        seq=seq,
        t_us=t_us,
        rssi=rssi,
        noise_floor=noise_floor,
        channel=channel,
        mac=mac,
        len=length,
        iq=iq,
    )


def verify_header_size() -> Tuple[int, int]:
    return CSI1_HDR_LEN, 29


if __name__ == "__main__":
    assert CSI1_HDR_LEN == 29, CSI1_HDR_LEN
    print(f"CSI1_HDR_LEN={CSI1_HDR_LEN} OK")
