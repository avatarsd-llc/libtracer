#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Assert a chip image does not link libstdc++'s shared atomic-wait table (#2507).

libstdc++ implements ``std::atomic::wait`` / ``notify_*`` (and ``std::atomic_flag::wait``) on
every target over one static table, ``std::__detail::__waiter_pool_base::_S_for(void const*)::__w``:
2,048 B of ``.bss`` on esp32c6. v0.22.0 grew a downstream esp32c6 image by exactly that when
the ESP-IDF WS client's teardown drain and the shared egress module started waiting with
``std::atomic::wait``. libtracer parks through ``tr::park_wait`` / ``tr::park_wake``
(``core/include/libtracer/park.hpp``) instead, and this check keeps the table from coming back.

GCC 16 reworked those internals: the table is ``__waitable_state``, reached through
``__wait_impl`` / ``__notify_impl``. The check matches the names of both eras, so a toolchain
bump cannot let the table back in unnoticed (#2510; ``tools/tests/test_check_no_waiter_pool.py``
feeds it a symbol list from each).

It reads the linked ELF, not the objects: the table is a weak, function-local static that
lands in the image when ANY translation unit instantiates a wait or a notify, so the image's
symbol table is the one place the cost is final. An image built on application code that
waits on an atomic itself would trip this too; ``full_node``, the example it runs on, does not.

Usage::

    tools/check_no_waiter_pool.py --elf integrations/esp-idf/examples/full_node/build/full_node.elf
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

# Demangled-name fragments of libstdc++'s atomic-wait machinery, per GCC era.
BANNED_FRAGMENTS = (
    # GCC 13-15: the header-only pool.
    "__waiter_pool",
    "__atomic_wait_address",
    # GCC 16: the reworked state table and its entry points.
    "__waitable_state",
    "__wait_impl",
    "__notify_impl",
)


def waiter_pool_hits(lines: list[str]) -> list[str]:
    """Return the ``nm -C`` lines that name libstdc++'s atomic-wait machinery, of either era."""
    return [ln for ln in lines if any(f in ln for f in BANNED_FRAGMENTS)]


def resolve_nm(explicit: str | None) -> str | None:
    """Pick the nm to read a cross-compiled ELF with: @p explicit, else the toolchain's."""
    for cand in (explicit, "riscv32-esp-elf-nm", "xtensa-esp32s3-elf-nm", "nm"):
        if cand and shutil.which(cand):
            return cand
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--elf", required=True, type=pathlib.Path, help="the linked chip image")
    ap.add_argument("--nm", default=None, help="nm binary that can read the image")
    args = ap.parse_args()

    if not args.elf.is_file():
        print(f"ERROR: {args.elf} is not a file", file=sys.stderr)
        return 2
    nm = resolve_nm(args.nm)
    if nm is None:
        print("ERROR: no usable nm found; source the IDF export script first", file=sys.stderr)
        return 2
    proc = subprocess.run([nm, "-C", "-S", str(args.elf)], capture_output=True, text=True)
    lines = proc.stdout.splitlines()
    if proc.returncode != 0 or len(lines) < 100:
        print(f"ERROR: {nm} read {len(lines)} symbol(s) from {args.elf}; an unreadable image "
              "must not read as 'no waiter table'", file=sys.stderr)
        return 2
    hits = waiter_pool_hits(lines)
    if hits:
        print("FAIL: the image links libstdc++'s atomic-wait table (2 KB of .bss on esp32c6). "
              "Park through tr::park_wait / tr::park_wake instead of std::atomic::wait:")
        for ln in hits:
            print(f"  {ln}")
        return 1
    print(f"OK: {len(lines)} symbols in {args.elf.name}, none from libstdc++'s atomic-wait table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
