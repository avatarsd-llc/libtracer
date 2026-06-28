#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Cross-core differential fuzzer for the libtracer wire codec.

The 12 curated vectors under ``vectors/v1/`` pin a handful of hand-picked wire
shapes. This catches the *drift between cores* those points miss: it generates
random **valid** frames from a seed, round-trips every frame through ALL THREE
native cores (C++ golden + TypeScript + Rust), and asserts byte-for-byte
agreement. A mismatch is a genuine cross-core bug — the failing seed + bytes are
printed so the frame can be promoted to a curated regression vector (and the run
exits non-zero).

Design (deterministic + reproducible):

  1. A self-contained generator builds, from an explicit integer seed, the
     *canonical* wire bytes of one random valid frame: a nonzero type code, a
     random legal opt-bit combo (reserved bits 0; TF only with TS and only on a
     nested node under an absolute-ts parent; LL/CW/CR/TS independent), correct
     length fields, random opaque payloads, and PL nesting kept well under the
     depth cap. The generator is a third, independent codec — it computes its own
     CRCs — so agreeing with it proves both cores agree with each other AND with
     the spec's canonical form, not merely with one another.

  2. Each core re-encodes via its ``--roundtrip`` batch hook (one hex frame per
     stdin line -> one re-encoded hex line, or ``ERR:<reason>``). For frame F the
     check is a three-way byte equality::

         F  ==  cpp(decode->encode F)  ==  ts(decode->encode F)  ==  rust(decode->encode F)

     Because F is canonical, each ``<core>_out == F`` is that core's ->*
     direction; their mutual equality is the cross-core gate (ADR-0028 /
     ADR-0032), now over thousands of shapes and three independent cores.

  3. N seeds (default 1000, ``--seeds``). On ANY mismatch the seed + every
     core's hex frame is printed and the run exits 1; otherwise a summary + exit 0.

Stdlib only. Reuses the harness-discovery convention of run-all.py (the C++
binary is found via $LIBTRACER_CXX_HARNESS or the common build paths).
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

# Same fallbacks run-all.py uses to locate the built C++ harness.
CXX_FALLBACKS = [
    REPO / "build" / "tests" / "conformance_runner",
    REPO / "core" / "build" / "tests" / "conformance_runner",
    Path("/tmp/lt-build/tests/conformance_runner"),
]
TS_HARNESS = REPO / "bindings" / "typescript" / "conformance" / "harness.mjs"
RUST_MANIFEST = REPO / "bindings" / "rust" / "Cargo.toml"
# The native Rust core runs as a cargo example (built on demand; no external deps).
RUST_CMD = [
    "cargo",
    "run",
    "--quiet",
    "--manifest-path",
    str(RUST_MANIFEST),
    "--example",
    "conformance",
    "--",
]

# Valid core type codes (0x05 retired). The codec accepts any nonzero type
# generically, so the generator also emits the occasional unknown/user code.
REGISTRY_TYPES = [0x01, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E]
MAX_DEPTH = 32  # codec depth cap; generated nesting stays well under it.
MAX_GEN_DEPTH = 6  # how deep this generator nests (must be < MAX_DEPTH).

# ------------------------------------------------------------------ crc ---
_CRC32C_TABLE = []
for _i in range(256):
    _c = _i
    for _k in range(8):
        _c = (0x82F63B78 ^ (_c >> 1)) if (_c & 1) else (_c >> 1)
    _CRC32C_TABLE.append(_c & 0xFFFFFFFF)

_CRC16_TABLE = []
for _i in range(256):
    _c = (_i << 8) & 0xFFFF
    for _k in range(8):
        _c = ((_c << 1) ^ 0x1021) & 0xFFFF if (_c & 0x8000) else (_c << 1) & 0xFFFF
    _CRC16_TABLE.append(_c)


def crc32c(data: bytes) -> int:
    """CRC-32C (Castagnoli): reflected poly 0x82F63B78, init/xor 0xFFFFFFFF."""
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC32C_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return (c ^ 0xFFFFFFFF) & 0xFFFFFFFF


def crc16ccitt(data: bytes) -> int:
    """CRC-16-CCITT (FALSE): poly 0x1021, MSB-first, init 0xFFFF, no final xor."""
    c = 0xFFFF
    for b in data:
        idx = ((c >> 8) ^ b) & 0xFF
        c = (_CRC16_TABLE[idx] ^ (c << 8)) & 0xFFFF
    return c


# ------------------------------------------------------------ generator ---
def _gen_node(rng: Random, depth: int) -> bytes:
    """Build the canonical wire bytes of one random valid TLV at `depth` (1 = root)."""
    # Mostly registry codes; sometimes any other nonzero byte to stress unknowns.
    type_b = rng.choice(REGISTRY_TYPES) if rng.random() < 0.85 else rng.randint(1, 0xFF)

    # Structured (PL) only while we have depth budget; bias toward leaves.
    structured = depth < MAX_GEN_DEPTH and rng.random() < 0.4

    pl = structured
    ll = rng.random() < 0.5  # u32 length field even when it would fit in u16
    ts = rng.random() < 0.5
    cr = rng.random() < 0.5
    cw = rng.random() < 0.5 if cr else False  # CRC-16 vs CRC-32C
    # TF (relative i32 ts) is only legal with TS=1 and only on a nested node
    # whose parent carries an absolute ts (the root is always absolute).
    tf = ts and depth > 1 and rng.random() < 0.5

    if structured:
        body = bytearray()
        for _ in range(rng.randint(0, 3)):
            body += _gen_node(rng, depth + 1)
        body = bytes(body)
    else:
        body = bytes(rng.randint(0, 0xFF) for _ in range(rng.randint(0, 16)))

    out = bytearray()
    opt = (
        (0x40 if pl else 0)
        | (0x20 if ts else 0)
        | (0x10 if cr else 0)
        | (0x08 if ll else 0)
        | (0x04 if cw else 0)
        | (0x02 if tf else 0)
    )
    out.append(type_b & 0xFF)
    out.append(opt)
    out += len(body).to_bytes(4 if ll else 2, "little")
    out += body

    ts_bytes = b""
    if ts:
        if tf:
            ts_bytes = rng.randint(-(2**31), 2**31 - 1).to_bytes(4, "little", signed=True)
        else:
            ts_bytes = rng.getrandbits(64).to_bytes(8, "little")
        out += ts_bytes

    if cr:
        covered = body + ts_bytes
        if cw:
            out += crc16ccitt(covered).to_bytes(2, "little")
        else:
            out += crc32c(covered).to_bytes(4, "little")

    return bytes(out)


def gen_frame(seed: int) -> bytes:
    """Deterministically generate the canonical bytes of one valid frame from `seed`."""
    return _gen_node(Random(seed), depth=1)


# --------------------------------------------------------------- driver ---
def find_cxx_harness() -> str | None:
    env = os.environ.get("LIBTRACER_CXX_HARNESS")
    if env and Path(env).exists():
        return env
    return next((str(p) for p in CXX_FALLBACKS if p.exists()), None)


def run_core(cmd: list[str], frames_hex: list[str]) -> list[str]:
    """Pipe all frame hex lines through a core's --roundtrip mode; return its lines."""
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
            f"(exit {proc.returncode}): {proc.stderr.strip()[:400]}"
        )
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--seeds", type=int, default=1000, help="number of seeds to fuzz (default 1000)")
    ap.add_argument("--start", type=int, default=1, help="first seed (default 1)")
    ap.add_argument("--max-fails", type=int, default=10, help="stop reporting after this many mismatches")
    args = ap.parse_args()

    cxx = find_cxx_harness()
    if cxx is None:
        print(
            "C++ harness not found. Build core/ (conformance_runner) or set "
            "$LIBTRACER_CXX_HARNESS.",
            file=sys.stderr,
        )
        return 2
    if not TS_HARNESS.exists():
        print(f"TS harness not found at {TS_HARNESS}", file=sys.stderr)
        return 2
    if not RUST_MANIFEST.exists():
        print(f"Rust core manifest not found at {RUST_MANIFEST}", file=sys.stderr)
        return 2

    seeds = list(range(args.start, args.start + args.seeds))
    frames = [gen_frame(s) for s in seeds]
    frames_hex = [f.hex() for f in frames]

    cpp_out = run_core([cxx, "--roundtrip"], frames_hex)
    ts_out = run_core(["node", str(TS_HARNESS), "--roundtrip"], frames_hex)
    rust_out = run_core(RUST_CMD + ["--roundtrip"], frames_hex)

    mismatches = 0
    for seed, want, cpp, ts, rust in zip(seeds, frames_hex, cpp_out, ts_out, rust_out):
        if want == cpp and want == ts and want == rust:
            continue
        mismatches += 1
        if mismatches <= args.max_fails:
            print(f"MISMATCH seed={seed}")
            print(f"  input : {want}")
            print(f"  cpp   : {cpp}{'' if cpp == want else '   <- DIVERGES'}")
            print(f"  ts    : {ts}{'' if ts == want else '   <- DIVERGES'}")
            print(f"  rust  : {rust}{'' if rust == want else '   <- DIVERGES'}")
            print(
                f"  reproduce: python3 tests/conformance/diff_fuzz.py --start {seed} --seeds 1"
            )

    print()
    print(f"diff-fuzz: {len(seeds)} seeds (start={args.start})  cpp={cxx}")
    if mismatches:
        print(f"DIFF-FUZZ: FAIL — {mismatches} mismatch(es); promote the seed(s) above to vectors")
        return 1
    print("DIFF-FUZZ: PASS — 0 mismatches; all cores agree byte-for-byte")
    return 0


if __name__ == "__main__":
    sys.exit(main())
