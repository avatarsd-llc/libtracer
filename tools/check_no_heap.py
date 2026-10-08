#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The MCU link check: what in ``libtracer.a`` still calls the heap (ADR-0083, #1783).

ADR-0083 Decision 1: every core allocation goes through the one injected seam, and the MCU
``libtracer.a`` references no heap at all. This reads the archive with ``nm`` and lists every
UNDEFINED reference to a heap entry point, per object:

* the C allocator: ``malloc``, ``calloc``, ``realloc``, ``free``, ``aligned_alloc``,
  ``posix_memalign``, ``memalign``, ``valloc``, ``pvalloc``, ``reallocarray``, ``strdup``,
  ``strndup``;
* C++: every ``operator new``, ``operator new[]``, ``operator delete`` and
  ``operator delete[]``, in every variant (sized, aligned, nothrow);
* the ESP-IDF and FreeRTOS heaps: ``heap_caps_*alloc*`` / ``heap_caps_free``,
  ``pvPortMalloc``, ``vPortFree``.

A deleting destructor counts: a class with a virtual destructor references
``operator delete`` from every object that emits its vtable, whether or not anything ever
deletes one, and the linker cannot tell the difference either.

* RATCHET. ``tools/no_heap_baseline.json`` pins, per target, the heap entry points each
  object still references, and the ticket that removes them. A reference the baseline does
  not list fails: a new heap call in an object, or a heap call in an object that had none.
  A pinned reference that is gone also fails until ``--repin`` drops it, so the baseline never
  claims more than the truth. ``--repin`` only removes pins; it never adds one.
* ZERO is the end state: an empty target in the baseline, and then this check is the plain
  "no heap symbol at all" gate ADR-0083 §Consequences names.

The objects are named as ``nm`` names archive members, so a baseline is per toolchain and
per build configuration. CI pins two: ``cortex-m0`` (the required-module archive
``tools/cortexm0_footprint.py --archive-out`` writes) and ``esp32c6`` (the ESP-IDF component
archive of ``integrations/esp-idf/examples/full_node``).

Usage::

    python3 tools/check_no_heap.py --target cortex-m0 --archive libtracer.a \\
        --nm arm-none-eabi-nm [--objdump arm-none-eabi-objdump]
    python3 tools/check_no_heap.py ... --repin      # drop pins that are gone
    python3 tools/check_no_heap.py ... --strict     # ignore the baseline: any reference fails

``--objdump`` adds, to each failure, the functions whose relocations name the symbol (with
``-ffunction-sections`` a function is its own section), which is where to start a fix.
Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "no_heap_baseline.json"

C_HEAP = (
    "malloc", "calloc", "realloc", "free", "aligned_alloc", "posix_memalign", "memalign",
    "valloc", "pvalloc", "reallocarray", "strdup", "strndup",
)
RTOS_HEAP = ("pvPortMalloc", "vPortFree")
HEAP_CAPS = re.compile(r"^heap_caps_\w*(?:alloc\w*|free)$")
# Itanium mangling: _Znw = operator new, _Zna = new[], _Zdl = delete, _Zda = delete[].
CXX_HEAP = {"_Znw": "operator new", "_Zna": "operator new[]",
            "_Zdl": "operator delete", "_Zda": "operator delete[]"}


def family(symbol: str) -> str | None:
    """@brief The heap entry point @p symbol names, or None when it names none."""
    if symbol in C_HEAP or symbol in RTOS_HEAP:
        return symbol
    if HEAP_CAPS.match(symbol):
        return symbol
    for prefix, name in CXX_HEAP.items():
        if symbol.startswith(prefix):
            return name
    return None


def parse_nm(text: str) -> dict[str, set[str]]:
    """@brief ``nm -A -u`` output -> {object: {heap entry point}}.

    Lines look like ``libtracer.a:graph.cpp.obj:         U _Znwj`` (GNU nm, ``-A``); a
    plain object file gives ``graph.o:         U _Znwj``. Only ``U`` (undefined) lines count.
    """
    found: dict[str, set[str]] = {}
    for line in text.splitlines():
        m = re.match(r"^(?P<where>.+?):\s+U\s+(?P<sym>\S+)$", line)
        if not m:
            continue
        fam = family(m.group("sym"))
        if fam is None:
            continue
        where = m.group("where")
        obj = where.rsplit(":", 1)[-1] if ":" in where else where
        found.setdefault(Path(obj).name, set()).add(fam)
    return found


def compare(found: dict[str, set[str]], pinned: dict[str, list[str]]):
    """@brief (new, gone): references the pins do not list, and pins no longer referenced."""
    new = {o: sorted(f - set(pinned.get(o, []))) for o, f in found.items()}
    gone = {o: sorted(set(p) - found.get(o, set())) for o, p in pinned.items()}
    return ({o: f for o, f in new.items() if f}, {o: f for o, f in gone.items() if f})


def attribute(objdump: str, archive: str, obj: str, fam: str) -> list[str]:
    """@brief Demangled names of the sections in @p obj whose relocations name @p fam."""
    try:
        res = subprocess.run([objdump, "-r", archive], capture_output=True, text=True)
    except OSError:
        return []
    sections: set[str] = set()
    member, section = None, None
    for line in res.stdout.splitlines():
        if line.endswith("file format") or "file format" in line:
            member = line.split(":", 1)[0].strip().rsplit("(", 1)[-1].rstrip(")")
            member = Path(member).name
        elif line.startswith("RELOCATION RECORDS FOR ["):
            section = line[len("RELOCATION RECORDS FOR ["):].rstrip("]:")
        elif member == obj and section:
            parts = line.split()
            if len(parts) >= 3 and family(parts[2].split("+")[0]) == fam:
                sections.add(section.split(".", 2)[-1] if section.startswith(".text.") else section)
    if not sections:
        return []
    try:
        dem = subprocess.run(["c++filt"], input="\n".join(sorted(sections)), capture_output=True,
                             text=True).stdout.splitlines()
    except OSError:
        dem = sorted(sections)
    return dem


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", required=True, help="baseline key (cortex-m0, esp32c6, ...)")
    ap.add_argument("--archive", required=True, help="the libtracer.a to read")
    ap.add_argument("--nm", default="nm", help="the target's nm (default: nm)")
    ap.add_argument("--objdump", default=None, help="the target's objdump, for attribution")
    ap.add_argument("--baseline", default=str(BASELINE), help="the pins (default: %(default)s)")
    ap.add_argument("--repin", action="store_true", help="drop pins that are gone, then pass")
    ap.add_argument("--strict", action="store_true", help="ignore the baseline: zero or fail")
    args = ap.parse_args()

    try:
        res = subprocess.run([args.nm, "-A", "-u", args.archive], capture_output=True, text=True)
    except OSError as exc:
        print(f"::error::cannot run {args.nm}: {exc}")
        return 1
    if res.returncode != 0:
        print(f"::error::{args.nm} failed on {args.archive}:\n{res.stderr.strip()}")
        return 1
    found = parse_nm(res.stdout)

    data = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
    entry = data.get("targets", {}).get(args.target)
    if entry is None and not args.strict:
        print(f"::error::no-heap: target {args.target!r} has no entry in {args.baseline}")
        return 1
    pinned: dict[str, list[str]] = {} if args.strict else entry.get("objects", {})
    new, gone = compare(found, pinned)

    total = sum(len(f) for f in found.values())
    print(f"no-heap [{args.target}]: {total} heap reference(s) in {len(found)} object(s) of "
          f"{args.archive}; {sum(len(f) for f in pinned.values())} pinned.")
    for obj in sorted(found):
        print(f"  {obj}: {', '.join(sorted(found[obj]))}")

    if args.repin and not new:
        if gone:
            for obj, fams in gone.items():
                left = sorted(set(pinned[obj]) - set(fams))
                if left:
                    pinned[obj] = left
                else:
                    del pinned[obj]
            Path(args.baseline).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
            print(f"repinned: dropped {sum(len(f) for f in gone.values())} pin(s)")
        return 0

    rc = 0
    for obj, fams in sorted(new.items()):
        rc = 1
        for fam in fams:
            print(f"::error::no-heap [{args.target}]: {obj} references {fam}, which "
                  f"libtracer.a must not (ADR-0083). Draw from the injected block source.")
            if args.objdump:
                for fn in attribute(args.objdump, args.archive, obj, fam):
                    print(f"    from {fn}")
    for obj, fams in sorted(gone.items()):
        rc = 1
        print(f"::error::no-heap [{args.target}]: {obj} no longer references "
              f"{', '.join(fams)}. Lower the pin: rerun with --repin.")
    if rc == 0:
        print("no-heap: OK — nothing beyond the pinned baseline.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
