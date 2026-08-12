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

The converse half depends on WHICH WS plane the packaging under test ships, selected
with ``--ws-plane``:

* ``native`` (the default — the ESP-IDF component): the IDF-native links must still be
  built, so a future over-trim that sheds the whole WS plane cannot pass as a fix.
* ``none`` (the PlatformIO ``espressif32`` packaging, #984): the ruled state is NO
  WebSocket at all — the portable pair is excluded per-environment by the
  ``library.json`` extra script, and the IDF-native links are not packaged for
  PlatformIO. Here the converse flips: the native-link TUs must be ABSENT too (their
  arrival is #984's sanctioned follow-up and must come with its own gate change), and
  their symbols join the nm assertion.

Object files are matched under both spellings — ``.cpp.obj`` (ESP-IDF's CMake/Ninja)
and ``.cpp.o`` (PlatformIO's SCons) — so a PlatformIO ``.pio/build/<env>`` tree is a
valid ``--build-dir``.

Usage::

    tools/check_esp_ws_plane.py --build-dir integrations/esp-idf/examples/full_node/build
    tools/check_esp_ws_plane.py --build-dir build --nm riscv32-esp-elf-nm
    tools/check_esp_ws_plane.py --build-dir tests/packaging/pio_esp32_can/.pio/build/esp32c6 \
        --ws-plane none --nm ~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm

Intended for a CHIP-target image. Do not run it against the ``linux`` target: there
the portable pair is the correct answer.
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

# The IDF-native WS link types — asserted absent from the image under --ws-plane none.
NATIVE_WS_SYMBOLS = ("httpd_ws_link", "esp_ws_client_link")

# TUs whose object file in the build tree means the portable pair was compiled.
PORTABLE_WS_TUS = ("transport_ws.cpp", "builtin_transport_ws.cpp")

# The IDF-native WS links that replace them in the ESP-IDF component (--ws-plane native).
NATIVE_WS_TUS = ("httpd_ws_link.cpp", "esp_ws_client_link.cpp")

# Both object spellings: ESP-IDF's CMake/Ninja emits <tu>.obj, PlatformIO's SCons <tu>.o.
OBJECT_SUFFIXES = (".obj", ".o")


def find_objects(build_dir: pathlib.Path, tu: str) -> list[pathlib.Path]:
    """Return every object compiled from @p tu under @p build_dir, either suffix."""
    hits: list[pathlib.Path] = []
    for suffix in OBJECT_SUFFIXES:
        hits.extend(build_dir.rglob(tu + suffix))
    return sorted(hits)

# Floor on the symbol table an nm read must produce before its "no match" counts as
# evidence. Not a tuned threshold — any real IDF app ELF lists thousands, and the only
# thing this separates is "searched a populated table" from "searched nothing".
MIN_PLAUSIBLE_SYMBOLS = 100


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
    ap.add_argument("--ws-plane", choices=("native", "none"), default="native",
                    help="which WS plane this packaging ships: 'native' (ESP-IDF "
                         "component — IDF links must be present) or 'none' (PlatformIO "
                         "espressif32, #984 — IDF links must be absent too)")
    args = ap.parse_args()

    build_dir: pathlib.Path = args.build_dir
    if not build_dir.is_dir():
        print(f"ERROR: {build_dir} is not a directory", file=sys.stderr)
        return 2

    ok = True

    # (1) Archive side: the portable TUs must not have been compiled at all.
    for tu in PORTABLE_WS_TUS:
        hits = find_objects(build_dir, tu)
        if hits:
            ok = False
            print(f"ERROR: {tu} was compiled into the chip build — the portable "
                  f"(POSIX-socket) WS transport is not excluded:", file=sys.stderr)
            for h in hits:
                print(f"    - {h}", file=sys.stderr)

    # (2) The converse, per --ws-plane. native: the IDF-native links must still be
    # there, so shedding the WHOLE WS plane cannot masquerade as the fix. none: the
    # ruled state IS no WS plane at all (#984), so a native link showing up means the
    # packaging grew a WS surface without the gate being updated alongside it.
    for tu in NATIVE_WS_TUS:
        hits = find_objects(build_dir, tu)
        if args.ws_plane == "native" and not hits:
            ok = False
            print(f"ERROR: {tu} object is absent — the IDF-native WS plane is missing, "
                  "so this build has no WebSocket at all (over-trim, not a fix).",
                  file=sys.stderr)
        elif args.ws_plane == "none" and hits:
            ok = False
            print(f"ERROR: {tu} was compiled — this packaging ships NO WS plane "
                  "(#984); packaging the IDF-native links is a separate change that "
                  "must update this gate:", file=sys.stderr)
            for h in hits:
                print(f"    - {h}", file=sys.stderr)

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
                symbols = proc.stdout.splitlines()
                # Non-vacuity: "no match" is only evidence if there was something to
                # match against. A stripped ELF, or an nm that read the file but
                # understood nothing, yields an empty table — and a substring search
                # over an empty table passes every assertion below. Fail instead.
                if len(symbols) < MIN_PLAUSIBLE_SYMBOLS:
                    ok = False
                    print(f"ERROR: {nm} listed only {len(symbols)} symbol(s) in "
                          f"{elf.name} — too few to have searched. A stripped or "
                          "unreadable ELF must not read as 'zero portable-WS symbols'.",
                          file=sys.stderr)
                # Under --ws-plane none the native link types are forbidden in the
                # image too — no WS plane means no WS plane, either implementation.
                forbidden = PORTABLE_WS_SYMBOLS
                if args.ws_plane == "none":
                    forbidden = forbidden + NATIVE_WS_SYMBOLS
                pattern = re.compile("|".join(forbidden))
                linked = [ln for ln in symbols if pattern.search(ln)]
                if linked:
                    ok = False
                    print(f"ERROR: {len(linked)} forbidden WS symbol(s) linked into "
                          f"{elf.name} — this image must carry ZERO symbols matching "
                          f"{' / '.join(forbidden)}:", file=sys.stderr)
                    for ln in linked[:20]:
                        print(f"    {ln}", file=sys.stderr)
                    if len(linked) > 20:
                        print(f"    ... and {len(linked) - 20} more", file=sys.stderr)
                elif len(symbols) >= MIN_PLAUSIBLE_SYMBOLS:
                    print(f"ok: 0 forbidden WS symbols among {len(symbols)} in "
                          f"{elf.name} (checked with {nm})")

    if ok:
        if args.ws_plane == "native":
            print("ok: the chip image carries the IDF-native WS plane only "
                  "(httpd_ws_link_t / esp_ws_client_link_t).")
        else:
            print("ok: the image carries NO WebSocket plane — neither the portable "
                  "POSIX pair nor the IDF-native links (#984).")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
