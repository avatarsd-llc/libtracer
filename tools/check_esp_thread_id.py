#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Assert a chip image's ownership stamps never call ``pthread_self`` (#1532).

v0.15.0 abort-looped at boot on ESP-IDF. ``transport_vertex_t``'s RFC-0014 S6 control-plane
seam stamped and checked mutex ownership with ``std::this_thread::get_id()``, which lowers to
``pthread_self()``. ESP-IDF's implementation looks the calling task up in its ``esp_pthread_t``
registry and **asserts** when it is absent::

    pthread_t pthread_self(void) {
        esp_pthread_t *pthread = pthread_find(xTaskGetCurrentTaskHandle());
        if (!pthread) { assert(false && "Failed to find current thread ID!"); }
        ...
    }

Any task created with ``xTaskCreate`` — the IDF **main task** running ``app_main`` included —
has no such registration, so the first control-plane transaction taken from one aborts the
device. Under the IDF default ``CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y`` that abort
carries no panic message: a bare reset loop, and on an OTA node a rollback.

**A successful build proves nothing here.** The offending call compiles and links perfectly for
every ESP target; the failure is at runtime, on silicon, and only from an unregistered task. So
this gate reads the objects instead, and it reads them two ways because either alone is weak:

* **Nothing in libtracer references ``pthread_self``.** Every libtracer object in the build tree
  is scanned for an undefined reference to it. This is the direct statement of the ruling.
* **The ownership-stamp TUs reference ``xTaskGetCurrentTaskHandle``.** Absence alone would also
  be satisfied by deleting the S6 self-check entirely, which is not the fix — the two-phase seam
  is platform-neutral and stays. Requiring the FreeRTOS primitive keeps an over-trim from
  passing as a repair.

Measured on ``full_node`` for esp32c6, this gate is a real differential: before the fix both
``transport_vertex.cpp.obj`` and ``self_heal_link.cpp.obj`` list ``U pthread_self``; after it,
neither does and both list ``U xTaskGetCurrentTaskHandle``.

``pthread_mutex_*`` references are expected and untouched — IDF implements those over FreeRTOS
semaphores and none of them consults the pthread registry. Only ``pthread_self`` is the hazard.

Usage::

    tools/check_esp_thread_id.py --build-dir integrations/esp-idf/examples/full_node/build
    tools/check_esp_thread_id.py --build-dir build --nm riscv32-esp-elf-nm

Intended for a CHIP-target image. Do not run it against the ``linux`` target: there
``std::this_thread::get_id()`` is the correct primitive and ``pthread_self`` is expected.
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

# The call that cannot be made from an unregistered FreeRTOS task.
BANNED_SYMBOL = "pthread_self"

# The primitive that replaces it — valid for pthread-created and native tasks alike.
REQUIRED_SYMBOL = "xTaskGetCurrentTaskHandle"

# The TUs that carry an ownership stamp, and therefore must show the replacement.
#
# A TU here may legitimately be ABSENT from a build: ``self_heal_link.cpp`` rides
# ``CONFIG_LIBTRACER_SELF_HEAL_LINKS`` since #1470, and a node that closed the link-liveness
# engine out compiles no such object. So the requirement is conditional on the object being
# PRESENT — a missing object is reported as configured-out, not as a stamp that was deleted.
# What is never optional is that at least one stamping TU was scanned: an image with none of
# them would make the whole "the stamp is ported, not gone" half vacuous, and
# ``transport_vertex.cpp`` is unconditional in every net-plane build, so that floor is real.
STAMPING_TUS = ("transport_vertex.cpp", "self_heal_link.cpp")

# Floor on the object count a scan must cover before its "no match" counts as evidence. Not a
# tuned threshold: a real full_node build compiles dozens of libtracer TUs, and the only thing
# this separates is "scanned a populated tree" from "scanned nothing" — the latter would make
# every absence assertion below vacuously green.
MIN_PLAUSIBLE_OBJECTS = 10


def find_libtracer_objects(build_dir: pathlib.Path) -> list[pathlib.Path]:
    """Every object compiled from a ``core/src`` TU under @p build_dir.

    ESP-IDF's CMake/Ninja mirrors the absolute source path under the component's
    ``CMakeFiles/<target>.dir`` tree, so a libtracer object's path contains ``core/src``. Both
    object spellings are matched (``.cpp.obj`` and ``.cpp.o``) for the same reason
    ``check_esp_ws_plane.py`` matches both.
    """
    hits: list[pathlib.Path] = []
    for pattern in ("*.cpp.obj", "*.cpp.o"):
        hits.extend(p for p in build_dir.rglob(pattern) if "core/src" in p.as_posix())
    return sorted(set(hits))


def resolve_nm(explicit: str | None) -> str | None:
    """Pick the nm to read a cross-compiled object with: @p explicit, else the toolchain's."""
    for cand in (explicit, "riscv32-esp-elf-nm", "xtensa-esp32s3-elf-nm", "nm"):
        if cand and shutil.which(cand):
            return cand
    return None


def undefined_symbols(nm: str, obj: pathlib.Path) -> list[str] | None:
    """The undefined symbols of @p obj, or None when nm could not read it."""
    proc = subprocess.run([nm, "-u", str(obj)], capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    return [line.split()[-1] for line in proc.stdout.splitlines() if line.strip()]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", required=True, type=pathlib.Path,
                    help="the idf.py build directory of a CHIP-target project")
    ap.add_argument("--nm", default=None, help="nm binary that can read the target objects")
    args = ap.parse_args()

    build_dir: pathlib.Path = args.build_dir
    if not build_dir.is_dir():
        print(f"ERROR: {build_dir} is not a directory", file=sys.stderr)
        return 2

    nm = resolve_nm(args.nm)
    if nm is None:
        print("ERROR: no usable nm found (tried --nm, riscv32-esp-elf-nm, "
              "xtensa-esp32s3-elf-nm, nm); source the IDF export script first", file=sys.stderr)
        return 2

    objects = find_libtracer_objects(build_dir)
    if len(objects) < MIN_PLAUSIBLE_OBJECTS:
        print(f"ERROR: found only {len(objects)} libtracer object(s) under {build_dir} — too "
              "few to have scanned. An empty tree must not read as 'no pthread_self'.",
              file=sys.stderr)
        return 1

    ok = True
    offenders: list[pathlib.Path] = []
    stamping_seen: dict[str, bool] = {tu: False for tu in STAMPING_TUS}
    stamping_present: dict[str, bool] = {tu: False for tu in STAMPING_TUS}

    for obj in objects:
        undef = undefined_symbols(nm, obj)
        if undef is None:
            ok = False
            print(f"ERROR: {nm} could not read {obj} — an unreadable object must not read as "
                  "a pass", file=sys.stderr)
            continue
        if BANNED_SYMBOL in undef:
            offenders.append(obj)
        for tu in STAMPING_TUS:
            if not obj.name.startswith(tu):
                continue
            stamping_present[tu] = True
            if REQUIRED_SYMBOL in undef:
                stamping_seen[tu] = True

    if offenders:
        ok = False
        print(f"ERROR: {len(offenders)} libtracer object(s) reference {BANNED_SYMBOL}, which "
              "ESP-IDF asserts out of for any task it did not register — the IDF main task "
              "included (#1532). Use tr::detail::this_thread_id() instead:", file=sys.stderr)
        for o in offenders:
            print(f"    - {o}", file=sys.stderr)

    for tu, seen in stamping_seen.items():
        if not stamping_present[tu]:
            # Configured out (#1470) — there is no object to carry a stamp. Say so in the log
            # rather than silently dropping the claim, so a build that lost the TU by accident
            # still leaves a trace someone can read.
            print(f"note: {tu} compiled no object in this build — configured out; its stamp "
                  "claim does not apply here.")
            continue
        if not seen:
            ok = False
            print(f"ERROR: {tu}'s object does not reference {REQUIRED_SYMBOL} — the ownership "
                  "stamp is gone rather than ported. Deleting the S6 self-check is not the "
                  "fix; the two-phase seam is platform-neutral and stays (#1532, #492).",
                  file=sys.stderr)

    if not any(stamping_present.values()):
        ok = False
        print("ERROR: none of the ownership-stamp TUs "
              f"({', '.join(STAMPING_TUS)}) compiled an object in this build. With none of "
              "them scanned the ported-stamp claim is vacuous, and transport_vertex.cpp is "
              "unconditional in every net-plane build — so this is a broken scan, not a "
              "trimmed one.", file=sys.stderr)

    if ok:
        scanned = [tu for tu, present in stamping_present.items() if present]
        print(f"ok: {len(objects)} libtracer objects scanned, zero reference {BANNED_SYMBOL}; "
              f"{len(scanned)} ownership-stamp TU(s) present ({', '.join(scanned)}) and each "
              f"uses {REQUIRED_SYMBOL}.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
