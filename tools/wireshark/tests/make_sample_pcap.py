#!/usr/bin/env python3
"""Emit a tiny sample.pcap of libtracer frames for eyeballing in the Wireshark GUI.

Each selected conformance-vector frame is wrapped in a synthetic
Ethernet/IPv4/TCP packet (dst port 47301) and written to tools/wireshark/sample.pcap.
Open it in Wireshark with libtracer.lua installed — the CRC-bearing frames are
claimed by the heuristic automatically; for the rest, set *Preferences → Protocols
→ libtracer → Raw TCP port* to 47301 (or right-click → Decode As → libtracer).

This is a convenience for humans; the authoritative regression test is run_tests.py.
"""
from __future__ import annotations

import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
VEC = REPO / "tests" / "conformance" / "vectors" / "v1"
OUT = REPO / "tools" / "wireshark" / "sample.pcap"
DST_PORT = 47301

# A readable cross-section of the protocol.
FRAMES = [
    "framing/empty-status-ok",
    "tlv-types/value-ts-abs",
    "crc/value-ts-abs-crc32c",
    "path/path-sensor-temp",
    "fwd/fwd-read",
    "fwd/fwd-write-value",
    "fwd/fwd-reply-result",
    "errors/error-registered-code",
]


def eth_ip_tcp(payload: bytes, seq: int) -> bytes:
    eth = bytes.fromhex("020000000002" "020000000001") + b"\x08\x00"
    tcp = struct.pack(
        ">HHIIBBHHH",
        50000, DST_PORT, seq, 0,
        0x50, 0x18, 0xFFFF, 0, 0,  # data-offset=5, flags=PSH|ACK, csum=0 (unchecked)
    ) + payload
    total = 20 + len(tcp)
    ip = struct.pack(
        ">BBHHHBBH4s4s",
        0x45, 0, total, 1, 0, 64, 6, 0,
        bytes([10, 0, 0, 2]), bytes([10, 0, 0, 1]),
    )
    return eth + ip + tcp


def main() -> None:
    packets = []
    seq = 1
    for case in FRAMES:
        raw = (VEC / case / "input.bin").read_bytes()
        packets.append(eth_ip_tcp(raw, seq))
        seq += len(raw)

    with open(OUT, "wb") as fh:
        # pcap global header: LINKTYPE_ETHERNET (1), snaplen 65535.
        fh.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for i, pkt in enumerate(packets):
            fh.write(struct.pack("<IIII", i, 0, len(pkt), len(pkt)))  # deterministic ts
            fh.write(pkt)
    print(f"wrote {OUT} ({len(packets)} packets)")


if __name__ == "__main__":
    main()
