#!/usr/bin/env python3
"""Regression-test the Wireshark libtracer dissector against the conformance vectors.

The dissector's byte-walking logic lives in tools/wireshark/libtracer.lua and is
exercised through its ``--decode-json`` CLI mode — the SAME code Wireshark runs,
no reimplementation. Each vector under tests/conformance/vectors/v1/ carries an
``input.bin`` (raw frame) and an ``expected.json`` (the reference decode); this
harness feeds every frame through the Lua decoder and asserts agreement.

Lua backend resolution, in order:
  1. a ``lua`` / ``lua5.4`` / ``luajit`` binary on PATH (the CI path), or $LUA;
  2. the ``lupa`` Python package (embedded Lua), if importable.
If neither is available the run SKIPs (exit 0) with a clear message, so the
harness never fails a machine that simply lacks a Lua runtime.

Usage: python3 tools/wireshark/tests/run_tests.py [--vectors DIR] [-v]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
LUA_FILE = REPO / "tools" / "wireshark" / "libtracer.lua"
DEFAULT_VECTORS = REPO / "tests" / "conformance" / "vectors" / "v1"


# --------------------------------------------------------------------------- #
# Lua backend
# --------------------------------------------------------------------------- #
class LuaBackend:
    """Decode a hex frame to the dissector's JSON via whatever Lua we can find."""

    def decode(self, hexstr: str) -> dict:
        raise NotImplementedError

    @staticmethod
    def find() -> "LuaBackend | None":
        for cand in (os.environ.get("LUA"), "lua", "lua5.4", "lua5.3", "luajit"):
            if cand and shutil.which(cand):
                return _BinaryLua(cand)
        try:
            import lupa  # noqa: F401
            return _LupaLua()
        except Exception:
            return None


class _BinaryLua(LuaBackend):
    def __init__(self, exe: str):
        self.exe = exe
        self.name = f"binary:{exe}"

    def decode(self, hexstr: str) -> dict:
        out = subprocess.run(
            [self.exe, str(LUA_FILE), "--decode-json", hexstr],
            capture_output=True, text=True, check=True,
        )
        return json.loads(out.stdout.strip())


class _LupaLua(LuaBackend):
    def __init__(self):
        from lupa import LuaRuntime
        self.L = LuaRuntime(unpack_returned_tuples=True)
        self.M = self.L.eval(f'dofile([[{LUA_FILE}]])')
        self.name = "lupa"

    def decode(self, hexstr: str) -> dict:
        # hex is pure ASCII, so it survives the Python<->Lua string boundary.
        return json.loads(self.M.decode_json(hexstr))


# --------------------------------------------------------------------------- #
# Comparison
# --------------------------------------------------------------------------- #
def type_code_from_expected(s: str) -> int:
    # expected.json spells types as e.g. "0x09 STATUS".
    return int(s.split()[0], 16)


def frame_bytes(case: Path, exp: dict) -> str:
    """Hex of the frame under test: input.bin | reject.bin | the expected `hex`."""
    for fn in ("input.bin", "reject.bin"):
        if (case / fn).exists():
            return (case / fn).read_bytes().hex().upper()
    return exp["hex"].upper()  # reject vectors ship bytes in the hex field only


def check_vector(case: Path, backend: LuaBackend, verbose: bool) -> list[str]:
    """Return a list of failure strings (empty == pass)."""
    exp = json.loads((case / "expected.json").read_text())
    hexstr = frame_bytes(case, exp)
    fails: list[str] = []

    got = backend.decode(hexstr)
    frame = got["frame"]

    # A `reject` vector is a FRAME-CODEC reject (reserved bit / sentinel): the
    # decoder MUST flag it invalid. Note `fwd-wildcard-reject` is NOT one of
    # these — it is codec-valid, rejected only at the graph resolution layer.
    reject = bool(exp.get("reject"))
    if reject:
        if not frame.get("invalid"):
            fails.append(f"reject vector ({exp['reject']}) was not flagged invalid")
        if verbose:
            print(f"  [{'PASS' if not fails else 'FAIL'}] "
                  f"{case.parent.name}/{case.name}: {got['summary']}")
        return fails

    # Byte accounting: for well-formed vectors the frame spans the whole file.
    if "total_bytes" in exp and frame.get("total_len") != exp["total_bytes"]:
        fails.append(f"total_len {frame.get('total_len')} != {exp['total_bytes']}")

    dec = exp.get("decoded") or {}

    if isinstance(dec.get("type"), str):
        want = type_code_from_expected(dec["type"])
        if frame.get("type") != want:
            fails.append(f"type 0x{frame.get('type'):02X} != 0x{want:02X}")

    if isinstance(dec.get("opt"), dict):
        for bit in ("PL", "TS", "CR", "LL", "CW", "TF"):
            if bit in dec["opt"] and frame["opt"].get(bit) != dec["opt"][bit]:
                fails.append(f"opt.{bit} {frame['opt'].get(bit)} != {dec['opt'][bit]}")

    if "length" in dec and frame.get("length") != dec["length"]:
        fails.append(f"length {frame.get('length')} != {dec['length']}")

    if isinstance(dec.get("payload"), str) and dec["payload"]:
        if (frame.get("payload_hex") or "").upper() != dec["payload"].upper():
            fails.append(f"payload {frame.get('payload_hex')} != {dec['payload']}")

    # Trailer CRC: compare width/value if given, and always require verification.
    exp_crc = (dec.get("trailer") or {}).get("crc")
    got_crc = (frame.get("trailer") or {}).get("crc")
    if exp_crc:
        if not got_crc:
            fails.append("expected a CRC trailer, decoder found none")
        else:
            if exp_crc.get("width") and got_crc["width"] != exp_crc["width"]:
                fails.append(f"crc width {got_crc['width']} != {exp_crc['width']}")
            if exp_crc.get("value") and int(got_crc["stored"], 16) != int(exp_crc["value"], 16):
                fails.append(f"crc value {got_crc['stored']} != {exp_crc['value']}")
    if got_crc and not got_crc["ok"]:
        fails.append(f"CRC verification FAILED (stored {got_crc['stored']} computed {got_crc['computed']})")

    # Children (PATH NAME segments etc.).
    if isinstance(dec.get("children"), list):
        gc = frame.get("children") or []
        if len(gc) != len(dec["children"]):
            fails.append(f"child count {len(gc)} != {len(dec['children'])}")
        else:
            for i, ec in enumerate(dec["children"]):
                if isinstance(ec.get("type"), str):
                    want = type_code_from_expected(ec["type"])
                    if gc[i].get("type") != want:
                        fails.append(f"child[{i}] type mismatch")
                if "length" in ec and gc[i].get("length") != ec["length"]:
                    fails.append(f"child[{i}] length {gc[i].get('length')} != {ec['length']}")
                if ec.get("payload_utf8"):
                    got_txt = bytes.fromhex(gc[i].get("payload_hex", "")).decode("utf-8", "replace")
                    if got_txt != ec["payload_utf8"]:
                        fails.append(f"child[{i}] utf8 {got_txt!r} != {ec['payload_utf8']!r}")

    if verbose:
        status = "PASS" if not fails else "FAIL"
        print(f"  [{status}] {case.parent.name}/{case.name}: {got['summary']}")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--vectors", default=str(DEFAULT_VECTORS))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    backend = LuaBackend.find()
    if backend is None:
        print("SKIP: no Lua runtime found (install lua5.4 or `pip install lupa`).")
        return 0
    print(f"Lua backend: {backend.name}")

    vroot = Path(args.vectors)
    cases = sorted(p.parent for p in vroot.rglob("expected.json"))
    if not cases:
        print(f"No vectors under {vroot}")
        return 1

    total, failed = 0, 0
    for case in cases:
        if not any((case / fn).exists() for fn in ("input.bin", "reject.bin")):
            continue
        total += 1
        try:
            fails = check_vector(case, backend, args.verbose)
        except Exception as e:  # decoder crash on a vector is itself a failure
            fails = [f"decoder raised: {e}"]
        if fails:
            failed += 1
            print(f"FAIL {case.parent.name}/{case.name}")
            for msg in fails:
                print(f"     - {msg}")

    print(f"\n{total - failed}/{total} vectors passed"
          + (f", {failed} FAILED" if failed else " — all green"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
