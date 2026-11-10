#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate the WebSocket-carrier Noise vectors (reference/23 §13.12, RFC-0033 Amendment 2).

Every Noise byte comes from `nnpsk0-appendix-a/transcript.json` (RFC-0033 Appendix A), so the
third-party checks of that case gate the Noise bytes here too. This script adds only RFC 6455
framing: a FIN bit, an opcode, a length and, on a client frame, a fixed masking key. Stdlib only.

    python3 tests/conformance/vectors/v1/noise/gen_ws_vectors.py           # rewrite the cases
    python3 tests/conformance/vectors/v1/noise/gen_ws_vectors.py --check   # fail if they differ

Each case is `ws-<name>/ws.json`. A frame lists `dir` (`c2s` client to server, `s2c` server to
client), `fin`, `opcode`, `mask` (hex, empty on a server frame), `payload` (the unmasked payload)
and `bytes` (the frame on the wire). The script parses every frame it writes back with a decoder
of its own and checks the reassembled messages before it writes anything.
"""
from __future__ import annotations

import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
A = json.loads((HERE / "nnpsk0-appendix-a" / "transcript.json").read_text())["outputs"]
MSG1, MSG2 = bytes.fromhex(A["msg1"]), bytes.fromhex(A["msg2"])
CONF, PING = bytes.fromhex(A["confirmation"]), bytes.fromhex(A["datagram_i2r"])
ACK, PANG = bytes.fromhex(A["acknowledgement"]), bytes.fromhex(A["datagram_r2i"])
SUBPROTOCOL = "tracer-noise-v1"
SOURCE = "RFC-0033 Amendment 2 A2.12; reference/23 §13.12"
BINARY, TEXT, CONT, PING_OP, PONG_OP = 0x2, 0x1, 0x0, 0x9, 0xA


class framer:
    """Client frames take the next fixed masking key: frame k (from 1) masks with a?, b?, c?, d?."""

    def __init__(self) -> None:
        self.k = 0

    def frame(self, d: str, payload: bytes, opcode: int = BINARY, fin: bool = True) -> dict:
        mask = b""
        if d == "c2s":
            self.k += 1
            mask = bytes([0xA0 + self.k, 0xB0 + self.k, 0xC0 + self.k, 0xD0 + self.k])
        n = len(payload)
        head = bytes([(0x80 if fin else 0) | opcode])
        mbit = 0x80 if mask else 0
        if n < 126:
            head += bytes([mbit | n])
        elif n < 65536:
            head += bytes([mbit | 126]) + n.to_bytes(2, "big")
        else:
            head += bytes([mbit | 127]) + n.to_bytes(8, "big")
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload)) if mask else payload
        return {"dir": d, "fin": fin, "opcode": opcode, "mask": mask.hex(), "payload": payload.hex(),
                "bytes": (head + mask + body).hex()}

    def split(self, d: str, msg: bytes, cuts: list[int], opcode: int = BINARY) -> list[dict]:
        """One message as continuation frames, cut at the given byte offsets."""
        edges = [0, *cuts, len(msg)]
        parts = [msg[a:b] for a, b in zip(edges, edges[1:])]
        return [self.frame(d, p, opcode if i == 0 else CONT, i == len(parts) - 1)
                for i, p in enumerate(parts)]


def parse(raw: bytes) -> dict:
    """An independent decode of one frame, for the self-check."""
    fin, op = bool(raw[0] & 0x80), raw[0] & 0x0F
    masked, n, i = bool(raw[1] & 0x80), raw[1] & 0x7F, 2
    if n == 126:
        n, i = int.from_bytes(raw[2:4], "big"), 4
    elif n == 127:
        n, i = int.from_bytes(raw[2:10], "big"), 10
    mask = raw[i:i + 4] if masked else b""
    i += len(mask)
    body = raw[i:]
    assert len(body) == n, "length field and payload disagree"
    payload = bytes(b ^ mask[j % 4] for j, b in enumerate(body)) if mask else body
    return {"fin": fin, "opcode": op, "masked": masked, "payload": payload}


def messages(frames: list[dict], d: str) -> list[bytes]:
    """Reassemble the data messages one direction carries; control frames pass between."""
    out, cur = [], None
    for f in frames:
        if f["dir"] != d:
            continue
        p = parse(bytes.fromhex(f["bytes"]))
        assert p["masked"] == (d == "c2s"), "a client masks every frame and a server none"
        assert p["payload"].hex() == f["payload"]
        if p["opcode"] >= 0x8:
            continue
        cur = p["payload"] if p["opcode"] != CONT else cur + p["payload"]
        if p["fin"]:
            out.append(cur)
            cur = None
    return out


def keys() -> dict:
    return {"k_i2r": A["k_i2r"], "k_r2i": A["k_r2i"], "handshake_hash": A["handshake_hash"]}


def case(name: str, summary: str, frames: list[dict], expected: dict, **inputs) -> dict:
    return {"case": f"noise/{name}", "source": SOURCE, "based_on": "noise/nnpsk0-appendix-a",
            "summary": summary, "subprotocol": SUBPROTOCOL, "inputs": inputs, "frames": frames,
            "expected": expected}


def prefix(f: framer) -> list[dict]:
    """The handshake and the empty confirmation and acknowledgement: proof is past."""
    return [f.frame("c2s", MSG1), f.frame("s2c", MSG2), f.frame("c2s", CONF), f.frame("s2c", ACK)]


def build() -> dict[str, dict]:
    cases: dict[str, dict] = {}

    f = framer()
    fr = [f.frame("c2s", MSG1), f.frame("s2c", MSG2), f.frame("c2s", CONF), f.frame("c2s", PING),
          f.frame("s2c", ACK), f.frame("s2c", PANG)]
    assert fr[0]["bytes"].startswith("82ba") and len(fr[0]["bytes"]) == 2 * 64
    assert fr[1]["bytes"].startswith("8232") and len(fr[1]["bytes"]) == 2 * 52
    assert messages(fr, "c2s") == [MSG1, CONF, PING] and messages(fr, "s2c") == [MSG2, ACK, PANG]
    cases["ws-handshake-transcript"] = case(
        "ws-handshake-transcript",
        "Appendix A over WebSocket: each datagram is one binary frame, client frames masked",
        fr, {"c2s_messages": [MSG1.hex(), CONF.hex(), PING.hex()],
             "s2c_messages": [MSG2.hex(), ACK.hex(), PANG.hex()], **keys()})

    f = framer()
    fr = (f.split("c2s", MSG1, [1, 33]) + f.split("s2c", MSG2, [25]) + f.split("c2s", CONF, [9])
          + f.split("c2s", PING, [1, 9, 25]) + f.split("s2c", ACK, [12])
          + f.split("s2c", PANG, [16]))
    assert messages(fr, "c2s") == [MSG1, CONF, PING] and messages(fr, "s2c") == [MSG2, ACK, PANG]
    cases["ws-fragmented"] = case(
        "ws-fragmented",
        "the same transcript, every message split into continuation frames",
        fr, {"c2s_messages": [MSG1.hex(), CONF.hex(), PING.hex()],
             "s2c_messages": [MSG2.hex(), ACK.hex(), PANG.hex()], **keys()},
        cuts={"msg1": [1, 33], "msg2": [25], "confirmation": [9], "datagram_i2r": [1, 9, 25],
              "acknowledgement": [12], "datagram_r2i": [16]})

    f = framer()
    fr = [f.frame("c2s", MSG1, TEXT)]
    cases["ws-text-opcode"] = case(
        "ws-text-opcode", "a valid first message sent as a text message is refused",
        fr, {"responder": {"malformed_rx": 1, "noise_handshake_failed": 0, "sent": [],
                           "connection_open": True}})

    max_frame = 16  # the bound after proof is max_frame + 25 = 41
    junk = bytes([0x04]) + bytes(range(1, 70))
    f = framer()
    head = prefix(f)
    over = f.split("c2s", junk[:70], [20, 40, 60])  # running totals 20, 40, 60 (> 41), 70
    tail = [f.frame("c2s", PING)]
    fr = head + over + tail
    cases["ws-oversize-fragments"] = case(
        "ws-oversize-fragments",
        "after proof, a fragmented message passes max_frame + 25 at its third fragment",
        fr, {"responder": {"malformed_rx": 1, "refused_at_frame": len(head) + 2,
                           "discarded_frames": [len(head) + 3], "delivered": [PING.hex()],
                           "sent": [MSG2.hex(), ACK.hex()], "connection_open": True}},
        max_frame=max_frame, bound_after_proof=max_frame + 25)

    f = framer()
    head = prefix(f)
    fr = head + [f.frame("c2s", junk[:max_frame + 26]), f.frame("c2s", PING)]
    cases["ws-oversize-single"] = case(
        "ws-oversize-single",
        "after proof, one unfragmented message one byte above max_frame + 25",
        fr, {"responder": {"malformed_rx": 1, "refused_at_frame": len(head),
                           "delivered": [PING.hex()], "sent": [MSG2.hex(), ACK.hex()],
                           "connection_open": True}},
        max_frame=max_frame, bound_after_proof=max_frame + 25)

    long1 = MSG1 + b"\x00"  # 59 bytes before any proof
    f = framer()
    whole = [f.frame("c2s", long1), f.frame("c2s", MSG1)]
    f = framer()
    frag = f.split("c2s", long1, [30]) + [f.frame("c2s", MSG1)]
    cases["ws-preproof-bound"] = case(
        "ws-preproof-bound",
        "before proof, a 59-byte message is refused at byte 59, whole or fragmented",
        whole + frag,
        {"variants": [
            {"frames": [0, 1], "refused_at_frame": 0, "malformed_rx": 1, "sent": [MSG2.hex()],
             "connection_open": True},
            {"frames": [2, 3, 4], "refused_at_frame": 3, "malformed_rx": 1, "sent": [MSG2.hex()],
             "connection_open": True}]},
        bound_before_proof=58,
        note="each variant runs on a fresh connection; the valid first message after the "
             "refusal is answered, so the refusal did not close the connection")

    f = framer()
    a, b = f.split("c2s", MSG1, [29])
    fr = [a, f.frame("c2s", b"", PING_OP), b, f.frame("s2c", b"", PONG_OP), f.frame("s2c", MSG2)]
    assert messages(fr, "c2s") == [MSG1]
    cases["ws-ping-between-continuations"] = case(
        "ws-ping-between-continuations",
        "a Ping between the two frames of a first message does not disturb its reassembly",
        fr, {"c2s_messages": [MSG1.hex()], "s2c_messages": [MSG2.hex()],
             "pong": fr[3]["bytes"]})
    return cases


def main() -> int:
    check = "--check" in sys.argv[1:]
    bad = 0
    for name, c in build().items():
        path = HERE / name / "ws.json"
        text = json.dumps(c, indent=2) + "\n"
        if check:
            if not path.exists() or path.read_text() != text:
                print(f"differs: {path.relative_to(HERE)}")
                bad = 1
            continue
        path.parent.mkdir(exist_ok=True)
        path.write_text(text)
        print(f"wrote {path.relative_to(HERE)}")
    return bad


if __name__ == "__main__":
    sys.exit(main())
