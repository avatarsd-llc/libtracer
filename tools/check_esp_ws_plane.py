#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Assert the ESP-IDF chip image carries the IDF-NATIVE WebSocket plane only.

Maintainer ruling on #947: **ESP-IDF WebSocket must never use POSIX sockets.**
``transport_ws_server`` / ``transport_ws_client`` (``core/src/transport_ws.cpp``) are
the HOST implementation — they own their socket and egress through
``posix_endpoint``'s ``sendmsg(MSG_NOSIGNAL)``, a flag lwIP defines but
``lwip_sendmsg`` rejects with ``EOPNOTSUPP``, so on silicon every scatter-gather data
frame is silently dropped while the handshake and PING/PONG still work (#948). The
sanctioned chip plane is ``httpd_ws_link_t`` (``esp_http_server``) and
``esp_ws_client_link_t`` (``esp_websocket_client``).

The component's CMakeLists therefore compiles the portable pair only on the ``linux``
target. This script is the gate that the exclusion REALLY happened, checked two ways
because either alone is weak:

* **Nothing compiled them** — ``transport_ws.cpp.obj`` / ``builtin_transport_ws.cpp.obj``
  are absent from the build tree (the archive-side proof).
* **Nothing links them** — ``nm`` on the final ELF reports ZERO
  ``transport_ws_server`` / ``transport_ws_client`` symbols (the image-side proof).
  Build-success alone proves nothing here: the pair COMPILES fine against lwIP, which
  is exactly why it shipped into images for so long. And ``--gc-sections`` cannot save
  the image on its own — the factory registration keeps the symbols reachable, which
  is why the before-figure on #947's consumer build was 46 linked symbols, not 0.

The converse half — the IDF-native links are still built — is asserted too, so a
future over-trim that sheds the whole WS plane cannot pass as a fix.

Usage::

    tools/check_esp_ws_plane.py --build-dir integrations/esp-idf/examples/full_node/build
    tools/check_esp_ws_plane.py --build-dir build --nm riscv32-esp-elf-nm

Intended for a CHIP-target build with ``CONFIG_LIBTRACER_TRANSPORT_WS=y``. Do not run
it against the ``linux`` target: there the portable pair is the correct answer.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

# The portable (POSIX-socket) WS types that must not reach a chip image. Matched as
# substrings of the MANGLED names nm prints — an Itanium-ABI mangling embeds each
# source identifier verbatim, so no demangler is needed (and none may be available).
PORTABLE_WS_SYMBOLS = ("transport_ws_server", "transport_ws_client")

# Object files whose presence in the build tree means the portable pair was compiled.
PORTABLE_WS_OBJECTS = ("transport_ws.cpp.obj", "builtin_transport_ws.cpp.obj")

# The IDF-native WS links that replace them on a chip target.
NATIVE_WS_OBJECTS = ("httpd_ws_link.cpp.obj", "esp_ws_client_link.cpp.obj")


def find_elf(build_dir: pathlib.Path) -> pathlib.Path | None:
    """Return the project ELF in @p build_dir (the one next to the flashable .bin)."""
    candidates = sorted(build_dir.glob("*.elf"))
    if not candidates:
        return None
    # A project build drops exactly one top-level ELF; bootloader/partition ELFs live
    # in their own subdirectories, which the non-recursive glob above already skips.
    return candidates[0]


def resolve_nm(explicit: str | None) -> str | None:
    """Pick the nm to read a RISC-V ELF with: @p explicit, else the IDF toolchain's."""
    for cand in (explicit, "riscv32-esp-elf-nm", "xtensa-esp32s3-elf-nm", "nm"):
        if cand and shutil.which(cand):
            return cand
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", required=True, type=pathlib.Path,
                    help="the idf.py build directory of a CHIP-target project")
    ap.add_argument("--nm", default=None, help="nm binary that can read the target ELF")
    args = ap.parse_args()

    build_dir: pathlib.Path = args.build_dir
    if not build_dir.is_dir():
        print(f"ERROR: {build_dir} is not a directory", file=sys.stderr)
        return 2

    ok = True

    # (1) Archive side: the portable TUs must not have been compiled at all.
    for obj in PORTABLE_WS_OBJECTS:
        hits = sorted(build_dir.rglob(obj))
        if hits:
            ok = False
            print(f"ERROR: {obj} was compiled into the chip build — the portable "
                  f"(POSIX-socket) WS transport is not excluded:", file=sys.stderr)
            for h in hits:
                print(f"    - {h}", file=sys.stderr)

    # (2) Converse: the IDF-native links must still be there, so shedding the WHOLE WS
    # plane cannot masquerade as this fix.
    for obj in NATIVE_WS_OBJECTS:
        if not any(build_dir.rglob(obj)):
            ok = False
            print(f"ERROR: {obj} is absent — the IDF-native WS plane is missing, so "
                  "this build has no WebSocket at all (over-trim, not a fix).",
                  file=sys.stderr)

    # (3) Image side: nm on the final ELF.
    elf = find_elf(build_dir)
    if elf is None:
        ok = False
        print(f"ERROR: no *.elf in {build_dir} — nothing to run nm against; a missing "
              "ELF must not read as a pass", file=sys.stderr)
    else:
        nm = resolve_nm(args.nm)
        if nm is None:
            ok = False
            print("ERROR: no usable nm found (tried --nm, riscv32-esp-elf-nm, "
                  "xtensa-esp32s3-elf-nm, nm); source the IDF export script first",
                  file=sys.stderr)
        else:
            proc = subprocess.run([nm, str(elf)], capture_output=True, text=True)
            if proc.returncode != 0:
                ok = False
                print(f"ERROR: {nm} failed on {elf}:\n{proc.stderr.strip()}",
                      file=sys.stderr)
            else:
                pattern = re.compile("|".join(PORTABLE_WS_SYMBOLS))
                linked = [ln for ln in proc.stdout.splitlines() if pattern.search(ln)]
                if linked:
                    ok = False
                    print(f"ERROR: {len(linked)} portable-WS symbol(s) linked into "
                          f"{elf.name} — the ESP-IDF image must carry ZERO "
                          "transport_ws_server / transport_ws_client symbols:",
                          file=sys.stderr)
                    for ln in linked[:20]:
                        print(f"    {ln}", file=sys.stderr)
                    if len(linked) > 20:
                        print(f"    ... and {len(linked) - 20} more", file=sys.stderr)
                else:
                    print(f"ok: 0 portable-WS symbols linked into {elf.name} "
                          f"(checked with {nm})")

    if ok:
        print("ok: the chip image carries the IDF-native WS plane only "
              "(httpd_ws_link_t / esp_ws_client_link_t).")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
