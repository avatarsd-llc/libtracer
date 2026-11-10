#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Parse every frame of the noise/ws-* cases with the third-party `websockets` library's own
RFC 6455 frame parser, and check each opcode, FIN bit and unmasked payload, and each reassembled
message, against what the case records. Run with `pip install websockets==13.1`; the result is in
independent_check_websockets.txt."""
import json
import pathlib

from websockets.frames import Frame
from websockets.streams import StreamReader

HERE = pathlib.Path(__file__).resolve().parent


def parse(raw: bytes, masked: bool) -> Frame:
    reader = StreamReader()
    reader.feed_data(raw)
    reader.feed_eof()
    gen = Frame.parse(reader.read_exact, mask=masked, max_size=None, extensions=[])
    try:
        next(gen)
    except StopIteration as done:
        return done.value
    raise AssertionError("frame incomplete")


def main() -> None:
    total = 0
    for path in sorted(HERE.glob("ws-*/ws.json")):
        case = json.loads(path.read_text())
        msgs = {"c2s": [], "s2c": []}
        cur = {"c2s": None, "s2c": None}
        for f in case["frames"]:
            fr = parse(bytes.fromhex(f["bytes"]), f["dir"] == "c2s")
            assert fr.opcode.value == f["opcode"] and fr.fin == f["fin"], path
            assert fr.data.hex() == f["payload"], path
            total += 1
            if fr.opcode.value >= 0x8:
                continue
            d = f["dir"]
            cur[d] = fr.data if fr.opcode.value != 0 else cur[d] + fr.data
            if fr.fin:
                msgs[d].append(cur[d].hex())
                cur[d] = None
        exp = case["expected"]
        for d in ("c2s", "s2c"):
            if f"{d}_messages" in exp:
                assert msgs[d] == exp[f"{d}_messages"], (path, d)
        print(f"ok  {case['case']}: {len(case['frames'])} frames")
    print(f"all {total} frames parse as recorded (websockets frame parser)")


if __name__ == "__main__":
    main()
