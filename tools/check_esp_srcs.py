#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Guard against ESP-IDF source-list drift.

``integrations/esp-idf/libtracer/CMakeLists.txt`` carries a HAND-MAINTAINED
``LIBTRACER_SRCS`` list of the ``core/src/*.cpp`` the ESP full-node build compiles.
It cannot simply reuse ``core/CMakeLists.txt``'s list — that one is assembled
CONDITIONALLY (NET_PLANE / per-transport / CUDA / QUIC / POSIX / socketcan-vs-stub
gates plus a generated ``builtin_transports.cpp``), whereas the ESP list is a flat
subset for one fixed config. So the two lists legitimately differ and can't share a
fragment.

The failure mode this guards: a NEW ``core/src/*.cpp`` that the ESP build needs is
added to ``core/CMakeLists.txt`` but forgotten in ``LIBTRACER_SRCS`` — surfacing only
as an ESP ``full-node`` link error much later (the project was "bitten twice"). This
check makes that drift LOUD at lint time: every core source is either referenced by
the ESP list or explicitly opted out below.
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE_SRC = ROOT / "core" / "src"
ESP_CMAKE = ROOT / "integrations" / "esp-idf" / "libtracer" / "CMakeLists.txt"

# core/src sources the ESP full-node build intentionally does NOT compile. Adding a
# new host-only / opt-in module? Add it here — a conscious opt-out, not silent drift.
EXCLUDED = {
    "mem_cuda.cpp",                # CUDA backend — host GPU only (LIBTRACER_WITH_CUDA)
    "transport_quic.cpp",          # QUIC — opt-in, separate libtracer_quic module (msquic)
    "transport_webtransport.cpp",  # WebTransport — QUIC-based, opt-in
}


def main() -> int:
    if not CORE_SRC.is_dir() or not ESP_CMAKE.is_file():
        print(f"error: expected {CORE_SRC} and {ESP_CMAKE} to exist", file=sys.stderr)
        return 2

    on_disk = {p.name for p in CORE_SRC.glob("*.cpp")}
    referenced = set(re.findall(r"core/src/([A-Za-z0-9_]+\.cpp)", ESP_CMAKE.read_text("utf-8")))

    expected = on_disk - EXCLUDED
    missing = sorted(expected - referenced)          # a core src the ESP build forgot
    ghost_refs = sorted(referenced - on_disk)        # ESP lists a core src that's gone
    stale_excludes = sorted(EXCLUDED - on_disk)      # EXCLUDED names a deleted file

    ok = True
    if missing:
        ok = False
        print(f"ERROR: {len(missing)} core/src source(s) missing from the ESP-IDF "
              f"LIBTRACER_SRCS list.\n  Add them to {ESP_CMAKE.relative_to(ROOT)}, or "
              "to EXCLUDED in this script if the ESP full-node build must not compile "
              "them:", file=sys.stderr)
        for m in missing:
            print(f"    - core/src/{m}", file=sys.stderr)
    if ghost_refs:
        ok = False
        print("ERROR: the ESP-IDF LIBTRACER_SRCS list references core sources that no "
              "longer exist (remove them):", file=sys.stderr)
        for g in ghost_refs:
            print(f"    - core/src/{g}", file=sys.stderr)
    if stale_excludes:
        ok = False
        print("ERROR: the EXCLUDED set names core sources that no longer exist "
              "(prune them):", file=sys.stderr)
        for s in stale_excludes:
            print(f"    - {s}", file=sys.stderr)

    if ok:
        print(f"ok: all {len(expected)} ESP-eligible core/src sources are in "
              f"LIBTRACER_SRCS ({len(EXCLUDED)} intentionally excluded).")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
