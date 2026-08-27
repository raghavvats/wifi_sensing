#!/usr/bin/env python3
"""Decode CSI1 / CSI2 binary UDP frames (see protocol/csi_frame.md)."""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass
from typing import List, Optional, Tuple, Union

CSI1_MAGIC = 0x43534931
CSI1_VERSION = 1
CSI1_HDR_FMT = "<IBBIQbbB6sH"
CSI1_HDR_LEN = struct.calcsize(CSI1_HDR_FMT)  # 29

CSI2_MAGIC = 0x43534932
CSI2_VERSION = 2
CSI2_HDR_FMT = "<IBBIQbbB6sIBBBIH"
CSI2_HDR_LEN = struct.calcsize(CSI2_HDR_FMT)  # 40


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


@dataclass
class Csi2Frame:
    magic: int
    version: int
    node_id: int
    seq: int
    t_us: int
    rssi: int
    noise_floor: int
    channel: int
    mac: bytes
    cycle_id: int
    slot_idx: int
    tx_node_id: int
    rx_node_id: int
    probe_seq: int
    len: int
    iq: bytes

    @property
    def mac_str(self) -> str:
        return ":".join(f"{b:02x}" for b in self.mac)

    @property
    def link_key(self) -> Tuple[int, int, int]:
        return (self.cycle_id, self.tx_node_id, self.rx_node_id)

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
            "magic": "CSI2",
            "version": self.version,
            "node_id": self.node_id,
            "seq": self.seq,
            "t_us": self.t_us,
            "rssi": self.rssi,
            "noise_floor": self.noise_floor,
            "channel": self.channel,
            "mac": self.mac_str,
            "cycle_id": self.cycle_id,
            "slot_idx": self.slot_idx,
            "tx_node_id": self.tx_node_id,
            "rx_node_id": self.rx_node_id,
            "probe_seq": self.probe_seq,
            "link": f"{self.tx_node_id}->{self.rx_node_id}",
            "len": self.len,
            "n_subcarriers": len(amps),
            "mean_amp": round(self.mean_amplitude(), 4),
            "amp": [round(a, 4) for a in amps],
        }


CsiFrame = Union[Csi1Frame, Csi2Frame]


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


def parse_csi2(data: bytes) -> Optional[Csi2Frame]:
    if len(data) < CSI2_HDR_LEN:
        return None
    (
        magic,
        version,
        node_id,
        seq,
        t_us,
        rssi,
        noise_floor,
        channel,
        mac,
        cycle_id,
        slot_idx,
        tx_node_id,
        rx_node_id,
        probe_seq,
        length,
    ) = struct.unpack(CSI2_HDR_FMT, data[:CSI2_HDR_LEN])
    if magic != CSI2_MAGIC or version != CSI2_VERSION:
        return None
    if length < 0 or CSI2_HDR_LEN + length > len(data):
        return None
    iq = data[CSI2_HDR_LEN : CSI2_HDR_LEN + length]
    return Csi2Frame(
        magic=magic,
        version=version,
        node_id=node_id,
        seq=seq,
        t_us=t_us,
        rssi=rssi,
        noise_floor=noise_floor,
        channel=channel,
        mac=mac,
        cycle_id=cycle_id,
        slot_idx=slot_idx,
        tx_node_id=tx_node_id,
        rx_node_id=rx_node_id,
        probe_seq=probe_seq,
        len=length,
        iq=iq,
    )


def parse_csi_frame(data: bytes) -> Optional[CsiFrame]:
    """Auto-detect CSI1 or CSI2 from magic."""
    if len(data) < 4:
        return None
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == CSI2_MAGIC:
        return parse_csi2(data)
    if magic == CSI1_MAGIC:
        return parse_csi1(data)
    return None


def verify_header_size() -> Tuple[int, int, int, int]:
    return CSI1_HDR_LEN, 29, CSI2_HDR_LEN, 40


if __name__ == "__main__":
    assert CSI1_HDR_LEN == 29, CSI1_HDR_LEN
    assert CSI2_HDR_LEN == 40, CSI2_HDR_LEN
    print(f"CSI1_HDR_LEN={CSI1_HDR_LEN} CSI2_HDR_LEN={CSI2_HDR_LEN} OK")
