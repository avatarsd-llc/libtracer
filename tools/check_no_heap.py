#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The MCU link check: what in ``libtracer.a`` still calls the heap (ADR-0083, #1783).

ADR-0083 Decision 1: every core allocation goes through the one injected seam, and the MCU
``libtracer.a`` references no heap at all. This reads the archive's relocations with
``objdump -r`` and lists every reference to a heap entry point, by CALL SITE: the object, the
section the reference sits in (with ``-ffunction-sections``, one function) and the entry point,
with how many times that section names it. The entry points:

* the C allocator: ``malloc``, ``calloc``, ``realloc``, ``free``, ``aligned_alloc``,
  ``posix_memalign``, ``memalign``, ``valloc``, ``pvalloc``, ``reallocarray``, ``strdup``,
  ``strndup``, and newlib's reentrant ``_malloc_r`` / ``_calloc_r`` / ``_realloc_r`` /
  ``_free_r`` / ``_memalign_r``;
* C++: every ``operator new``, ``operator new[]``, ``operator delete`` and
  ``operator delete[]``, in every variant (sized, aligned, nothrow);
* the ESP-IDF and FreeRTOS heaps: ``heap_caps_*alloc*`` / ``heap_caps_free``,
  ``pvPortMalloc``, ``vPortFree``.

A deleting destructor counts: a class with a virtual destructor references
``operator delete`` from every object that emits its vtable, whether or not anything ever
deletes one, and the linker cannot tell the difference either.

* RATCHET. ``tools/no_heap_baseline.json`` pins, per target, every call site that references
  the heap today: ``{object: {section: {entry point: count}}}``. A site the baseline does not
  list fails, and so does a pinned site whose count grew: a heap call in a new function, or a
  second one in a function that already had one. A pinned site that is gone or shrank also
  fails until ``--repin`` lowers it, so the baseline never claims more than the truth.
  ``--repin`` only lowers pins; it never adds one.
* ZERO is the end state: an empty target in the baseline, and then this check is the plain
  "no heap symbol at all" gate ADR-0083 §Consequences names. ``--strict`` runs that gate now.

The objects and sections are named as the toolchain names them, so a baseline is per toolchain
and per build configuration. CI pins two: ``cortex-m0`` (the required-module archive
``tools/cortexm0_footprint.py --archive-out`` writes) and ``esp32c6`` (the ESP-IDF component
archive of ``integrations/esp-idf/examples/full_node``). ``--seed`` writes the pins for a
target the baseline does not have yet, and refuses one it has.

Usage::

    python3 tools/check_no_heap.py --target cortex-m0 --archive libtracer.a \\
        --objdump arm-none-eabi-objdump
    python3 tools/check_no_heap.py ... --repin      # lower pins that are gone
    python3 tools/check_no_heap.py ... --strict     # ignore the baseline: any reference fails

Stdlib only; ``c++filt``, when present, demangles the section names in messages.
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
    "_malloc_r", "_calloc_r", "_realloc_r", "_free_r", "_memalign_r",
)
RTOS_HEAP = ("pvPortMalloc", "vPortFree")
HEAP_CAPS = re.compile(r"^heap_caps_\w*(?:alloc\w*|free)$")
# Itanium mangling: _Znw = operator new, _Zna = new[], _Zdl = delete, _Zda = delete[].
CXX_HEAP = {"_Znw": "operator new", "_Zna": "operator new[]",
            "_Zdl": "operator delete", "_Zda": "operator delete[]"}

# {object: {section: {entry point: count}}}
Sites = dict[str, dict[str, dict[str, int]]]


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


def short_section(section: str) -> str:
    """@brief A function section's symbol (``.text.<sym>`` -> ``<sym>``); others unchanged."""
    return section[len(".text."):] if section.startswith(".text.") else section


def parse_relocs(text: str) -> Sites:
    """@brief ``objdump -r`` output -> {object: {section: {entry point: count}}}.

    An archive's member starts at ``<member>:     file format <fmt>``; each section's
    relocations at ``RELOCATION RECORDS FOR [<section>]:``; each record is
    ``<offset> <type> <symbol>[+<addend>]``. Relocation sections that are themselves
    metadata (debug info, unwind tables) are skipped: they name a function, not a call.
    """
    sites: Sites = {}
    member, section = None, None
    for line in text.splitlines():
        if "file format" in line:
            member = Path(line.split(":", 1)[0].strip()).name
            section = None
        elif line.startswith("RELOCATION RECORDS FOR ["):
            section = line[len("RELOCATION RECORDS FOR ["):].rstrip(":").rstrip("]")
            if section.startswith((".debug", ".eh_frame", ".ARM.ex", ".ARM.extab")):
                section = None
        elif member and section:
            parts = line.split()
            if len(parts) < 3:
                continue
            fam = family(parts[2].split("+")[0])
            if fam is None:
                continue
            per = sites.setdefault(member, {}).setdefault(short_section(section), {})
            per[fam] = per.get(fam, 0) + 1
    return sites


def flatten(sites: Sites) -> dict[tuple[str, str, str], int]:
    """@brief {(object, section, entry point): count}."""
    return {(o, s, f): n for o, secs in sites.items() for s, fams in secs.items()
            for f, n in fams.items()}


def compare(found: Sites, pinned: Sites):
    """@brief (new, gone): sites above their pin, and pins above what is found.

    Each is {(object, section, entry point): (pinned, found)}.
    """
    f, p = flatten(found), flatten(pinned)
    new = {k: (p.get(k, 0), n) for k, n in f.items() if n > p.get(k, 0)}
    gone = {k: (n, f.get(k, 0)) for k, n in p.items() if n > f.get(k, 0)}
    return new, gone


def lower(pinned: Sites, found: Sites) -> Sites:
    """@brief @p pinned with every pin lowered to what is @p found (never raised)."""
    f = flatten(found)
    out: Sites = {}
    for (o, s, fam), n in sorted(flatten(pinned).items()):
        keep = min(n, f.get((o, s, fam), 0))
        if keep:
            out.setdefault(o, {}).setdefault(s, {})[fam] = keep
    return out


def demangle(names: list[str]) -> dict[str, str]:
    """@brief Best-effort ``c++filt`` over @p names; a name it cannot read maps to itself."""
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True,
                             text=True).stdout.splitlines()
    except OSError:
        return {n: n for n in names}
    return dict(zip(names, out)) if len(out) == len(names) else {n: n for n in names}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", required=True, help="baseline key (cortex-m0, esp32c6, ...)")
    ap.add_argument("--archive", required=True, help="the libtracer.a to read")
    ap.add_argument("--objdump", default="objdump", help="the target's objdump")
    ap.add_argument("--baseline", default=str(BASELINE), help="the pins (default: %(default)s)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--repin", action="store_true", help="lower pins that are gone, then pass")
    mode.add_argument("--strict", action="store_true", help="ignore the baseline: zero or fail")
    mode.add_argument("--seed", metavar="SOURCE",
                      help="pin every site for a target the baseline lacks; SOURCE says what "
                           "build the pins came from")
    args = ap.parse_args()

    try:
        res = subprocess.run([args.objdump, "-r", args.archive], capture_output=True, text=True)
    except OSError as exc:
        print(f"::error::cannot run {args.objdump}: {exc}")
        return 1
    if res.returncode != 0:
        print(f"::error::{args.objdump} failed on {args.archive}:\n{res.stderr.strip()}")
        return 1
    found = parse_relocs(res.stdout)

    path = Path(args.baseline)
    data = json.loads(path.read_text(encoding="utf-8"))
    targets = data.setdefault("targets", {})
    entry = targets.get(args.target)
    if args.seed:
        if entry is not None:
            print(f"::error::no-heap: target {args.target!r} is already pinned; --seed only "
                  f"starts a new one")
            return 1
        targets[args.target] = {"source": args.seed, "sites": found}
        path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"seeded {args.target}: {sum(flatten(found).values())} reference(s)")
        return 0
    if entry is None and not args.strict:
        print(f"::error::no-heap: target {args.target!r} has no entry in {args.baseline}")
        return 1
    pinned: Sites = {} if args.strict else entry.get("sites", {})
    new, gone = compare(found, pinned)

    flat = flatten(found)
    print(f"no-heap [{args.target}]: {sum(flat.values())} heap reference(s) at {len(flat)} "
          f"site(s) in {len(found)} object(s) of {args.archive}; "
          f"{sum(flatten(pinned).values())} pinned.")

    if args.repin and not new:
        if gone:
            entry["sites"] = lower(pinned, found)
            path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            print(f"repinned: lowered {len(gone)} site(s)")
        return 0

    names = demangle(sorted({s for (_, s, _) in list(new) + list(gone)}))
    for (obj, sec, fam), (was, now) in sorted(new.items()):
        print(f"::error::no-heap [{args.target}]: {obj} calls {fam} from {names.get(sec, sec)} "
              f"({now} reference(s), {was} pinned), which libtracer.a must not (ADR-0083). "
              f"Draw from the injected block source.")
    for (obj, sec, fam), (was, now) in sorted(gone.items()):
        print(f"::error::no-heap [{args.target}]: {obj} {names.get(sec, sec)} references {fam} "
              f"{now} time(s), {was} pinned. Lower the pin: rerun with --repin.")
    if new or gone:
        return 1
    print("no-heap: OK — nothing beyond the pinned baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
