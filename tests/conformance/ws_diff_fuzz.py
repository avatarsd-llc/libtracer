#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Differential fuzzer for the RFC 6455 WebSocket frame DECODER (#60 / hardening).

The ws frame layer is network-facing attack surface: it parses untrusted bytes
(FIN/opcode, the 7/16/64-bit length encodings, the client mask, and the
overflow-safe 64-bit-over-long path) BEFORE the libtracer TLV layer ever sees
them. The curated vectors in core/tests/ws_test.cpp /
transport-ws/test/ws-codec.test.mjs pin a handful of shapes; this catches the
drift between the two cores those points miss — and, just as importantly, proves
NEITHER decoder crashes / reads out of bounds on any input.

It mirrors tests/conformance/diff_fuzz.py (the TLV wire-codec differential
fuzzer), but over two impls instead of three:

  * C++  tr::net::ws::decode_frame   via core/tests/ws_fuzz_harness.cpp, and
  * TS   decodeFrame                 via transport-ws/fuzz/decode_harness.mjs.

Both harnesses speak one canonical decode line per hex-frame stdin line:

    OK\\t<opcode>\\t<fin>\\t<consumed>\\t<payload-hex>   full decode
    NEED_MORE                                          decode_frame -> nullopt/null
    ERR:<reason>                                       non-hex / empty line

For each seed a self-contained, deterministic generator builds one frame byte
sequence — a MIX of well-formed frames (random opcode incl. TEXT/BINARY/CLOSE/
PING/PONG, random payloads, masked + unmasked, all three length encodings incl.
the 126/127 markers, non-minimal encodings) and ADVERSARIAL/malformed inputs
(truncated at every boundary, a 64-bit length with the high bit set / claiming
huge sizes on a short buffer, mask-bit-set-but-missing-key, reserved bits set,
zero-length, and multi-frame buffers). The same bytes go to BOTH harnesses and
their canonical lines must AGREE. On ANY mismatch the seed + frame hex are printed
(so the frame can be promoted to a curated regression vector) and the run exits
non-zero; a harness that crashes or returns the wrong number of lines is likewise
a failure. Stdlib only; deterministic (seed-driven — no time / os.urandom).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from random import Random

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent  # tests/conformance -> repo root

# Same locate-the-built-binary convention as diff_fuzz.py's CXX_FALLBACKS.
CXX_FALLBACKS = [
    REPO / "build" / "tests" / "ws_fuzz_harness",
    REPO / "core" / "build" / "tests" / "ws_fuzz_harness",
    Path("/tmp/lt-build/tests/ws_fuzz_harness"),
]
TS_HARNESS = REPO / "bindings" / "typescript" / "packages" / "transport-ws" / "fuzz" / "decode_harness.mjs"
TS_DIST = REPO / "bindings" / "typescript" / "packages" / "transport-ws" / "dist" / "ws.js"

# RFC 6455 opcodes libtracer names; the generator also emits arbitrary 4-bit codes
# so the decoders are exercised on unknown opcodes too (both just pass them through).
NAMED_OPCODES = [0x0, 0x1, 0x2, 0x8, 0x9, 0xA]


# ------------------------------------------------------------ generator ---
def _build(b0: int, mask_bit: int, declared_len: int, len_field: str, mask_key: bytes,
           body: bytes) -> bytes:
    """Low-level frame assembler with full control over each wire field, so the
    adversarial cases (non-minimal lengths, a declared length that disagrees with
    the body, a missing mask key) can be expressed directly."""
    out = bytearray([b0 & 0xFF])
    if len_field == "7":
        out.append((mask_bit | (declared_len & 0x7F)) & 0xFF)
    elif len_field == "16":
        out.append((mask_bit | 126) & 0xFF)
        out += (declared_len & 0xFFFF).to_bytes(2, "big")
    else:  # "64"
        out.append((mask_bit | 127) & 0xFF)
        out += (declared_len & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "big")
    if mask_bit:
        out += mask_key
    out += body
    return bytes(out)


def _well_formed(rng: Random, *, masked=None, opcode=None, payload=None, len_field=None,
                 rsv=0, fin=None) -> bytes:
    """One internally-consistent RFC 6455 frame: the declared length matches the
    body, a masked frame XORs the payload under its 4-byte key. A non-minimal
    length encoding (a small payload sent under the 126/127 marker) is allowed —
    the decoder must accept it, and both cores must agree it does."""
    if fin is None:
        fin = rng.random() < 0.8
    if opcode is None:
        opcode = rng.choice(NAMED_OPCODES) if rng.random() < 0.7 else rng.randint(0, 0xF)
    if masked is None:
        masked = rng.random() < 0.5
    if payload is None:
        # A spread that straddles the 7-bit / 126 / (occasionally) 65535 boundaries.
        n = rng.choice([0, 1, 5, 125, 126, 127, rng.randint(0, 300)])
        payload = bytes(rng.randint(0, 255) for _ in range(n))
    n = len(payload)
    if len_field is None:
        opts = ["min"]
        if n < 126:
            opts += ["16", "64"]
        elif n <= 0xFFFF:
            opts += ["64"]
        len_field = rng.choice(opts)
    if len_field == "min":
        len_field = "7" if n < 126 else ("16" if n <= 0xFFFF else "64")

    b0 = (0x80 if fin else 0) | ((rsv & 0x7) << 4) | (opcode & 0x0F)
    if masked:
        mk = bytes(rng.randint(0, 255) for _ in range(4))
        body = bytes(payload[i] ^ mk[i % 4] for i in range(n))
        return _build(b0, 0x80, n, len_field, mk, body)
    return _build(b0, 0x00, n, len_field, b"", payload)


def gen_frame(seed: int) -> bytes:
    """Deterministically build one frame byte sequence from `seed`. Picks a
    category — well-formed (masked / unmasked / reserved-bits / zero-length /
    non-minimal length) or adversarial (truncated at a boundary, a 64-bit
    over-long length, a missing mask key, a multi-frame buffer, raw noise)."""
    rng = Random(seed)
    cat = rng.choice([
        "wf_unmasked", "wf_masked", "wf_reserved", "wf_zero", "wf_control",
        "truncated", "overlong64", "mask_missing", "multi_frame", "noise",
    ])

    if cat == "wf_unmasked":
        return _well_formed(rng, masked=False)
    if cat == "wf_masked":
        return _well_formed(rng, masked=True)
    if cat == "wf_reserved":
        return _well_formed(rng, rsv=rng.randint(1, 7))
    if cat == "wf_zero":
        return _well_formed(rng, payload=b"")
    if cat == "wf_control":
        # CLOSE/PING/PONG, short payloads, masked (as a real client would send).
        return _well_formed(rng, masked=True, opcode=rng.choice([0x8, 0x9, 0xA]),
                            payload=bytes(rng.randint(0, 255) for _ in range(rng.randint(0, 8))))

    if cat == "truncated":
        # A complete frame chopped at an arbitrary boundary (header, length field,
        # mask key, or mid-payload). Both decoders must say NEED_MORE.
        full = _well_formed(rng, payload=bytes(rng.randint(0, 255) for _ in range(rng.randint(1, 260))))
        if len(full) <= 1:
            return full
        return full[: rng.randint(1, len(full) - 1)]

    if cat == "overlong64":
        # 127 marker with a declared length far larger than the buffer — the high
        # bit may be set (~2^63+). The overflow-safe bounds check must reject this
        # (NEED_MORE) without ever reading past the buffer.
        b0 = (0x80 if rng.random() < 0.8 else 0) | rng.choice(NAMED_OPCODES)
        masked = rng.random() < 0.5
        mask_bit = 0x80 if masked else 0
        mk = bytes(rng.randint(0, 255) for _ in range(4)) if masked else b""
        declared = rng.choice([
            0xFFFFFFFFFFFFFFFF,            # all ones
            0x8000000000000000,            # high bit set, nothing else
            rng.randint(2**32, 2**63),     # huge but high-bit-clear
            rng.randint(1000, 100000),     # merely larger than the short buffer
        ])
        body = bytes(rng.randint(0, 255) for _ in range(rng.randint(0, 6)))
        return _build(b0, mask_bit, declared, "64", mk, body)

    if cat == "mask_missing":
        # MASK bit set but the 4-byte key is absent or truncated -> NEED_MORE.
        b0 = 0x80 | rng.choice(NAMED_OPCODES)
        n = rng.randint(0, 40)
        keep = rng.randint(0, 3)  # 0..3 mask-key bytes present (never the full 4)
        return _build(b0, 0x80, n, "7", bytes(rng.randint(0, 255) for _ in range(keep)), b"")

    if cat == "multi_frame":
        # Two or three concatenated well-formed frames; the decoder consumes only
        # the first. Both cores must report the same opcode/fin/consumed/payload.
        out = bytearray()
        for _ in range(rng.randint(2, 3)):
            out += _well_formed(rng, payload=bytes(rng.randint(0, 255) for _ in range(rng.randint(0, 8))))
        return bytes(out)

    # "noise": short raw bytes — exercises the 2-byte minimum + arbitrary headers.
    return bytes(rng.randint(0, 255) for _ in range(rng.randint(1, 12)))


# --------------------------------------------------------------- driver ---
def _find_cpp() -> str | None:
    env = os.environ.get("LIBTRACER_WS_FUZZ_HARNESS")
    if env and Path(env).exists():
        return env
    return next((str(p) for p in CXX_FALLBACKS if p.exists()), None)


def run_core(cmd: list[str], frames_hex: list[str]) -> list[str]:
    """Pipe every frame hex line through a harness; return its output lines. A
    crash (wrong line count / non-zero exit with truncated output) raises."""
    proc = subprocess.run(
        cmd,
        input="\n".join(frames_hex) + "\n",
        capture_output=True,
        text=True,
        cwd=REPO,
    )
    out = proc.stdout.splitlines()
    if len(out) != len(frames_hex):
        raise RuntimeError(
            f"{cmd[0]} returned {len(out)} lines for {len(frames_hex)} frames "
            f"(exit {proc.returncode}) — possible crash/OOB. stderr: "
            f"{proc.stderr.strip()[:400]}"
        )
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("-n", "--seeds", type=int, default=2000, help="number of seeds to fuzz (default 2000)")
    ap.add_argument("--start", type=int, default=1, help="first seed (default 1)")
    ap.add_argument("--max-fails", type=int, default=10, help="stop reporting after this many mismatches")
    args = ap.parse_args()

    cpp = _find_cpp()
    if cpp is None:
        print(
            "C++ ws fuzz harness not found. Build it (cmake --build <dir> --target "
            "ws_fuzz_harness) or set $LIBTRACER_WS_FUZZ_HARNESS.",
            file=sys.stderr,
        )
        return 2
    if not TS_HARNESS.exists():
        print(f"TS harness not found at {TS_HARNESS}", file=sys.stderr)
        return 2
    if not TS_DIST.exists():
        print(
            f"TS codec not built ({TS_DIST} missing). Run `npm run build` in "
            "bindings/typescript (or the transport-ws package).",
            file=sys.stderr,
        )
        return 2

    cpp_cmd = [cpp]
    ts_cmd = ["node", str(TS_HARNESS)]

    seeds = list(range(args.start, args.start + args.seeds))
    frames = [gen_frame(s) for s in seeds]
    frames_hex = [f.hex() for f in frames]

    cpp_out = run_core(cpp_cmd, frames_hex)
    ts_out = run_core(ts_cmd, frames_hex)

    mismatches = 0
    for seed, frame_hex, c, t in zip(seeds, frames_hex, cpp_out, ts_out):
        if c == t:
            continue
        mismatches += 1
        if mismatches <= args.max_fails:
            print(f"MISMATCH seed={seed}")
            print(f"  frame : {frame_hex}")
            print(f"  cpp   : {c!r}")
            print(f"  ts    : {t!r}")
            print(f"  reproduce: python3 tests/conformance/ws_diff_fuzz.py --start {seed} --seeds 1")

    print()
    print(f"ws-diff-fuzz: {len(seeds)} seeds (start={args.start})  cpp={cpp}")
    if mismatches:
        print(f"WS-DIFF-FUZZ: FAIL — {mismatches} mismatch(es); promote the seed(s) above to vectors")
        return 1
    print("WS-DIFF-FUZZ: PASS — 0 mismatches; C++ and TS decoders agree, neither crashed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
