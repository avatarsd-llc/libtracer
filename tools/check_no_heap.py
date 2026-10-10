#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The MCU link check: what in ``libtracer.a`` still calls the heap (ADR-0083, #1783).

ADR-0083 Decision 1: every core allocation goes through the one injected seam, and the MCU
``libtracer.a`` reaches no heap at all. This reads the archive's symbols and relocations with
``objdump -r -t`` and lists every function that REACHES a heap entry point, by call site: the
object, the section (with ``-ffunction-sections``, one function) and what it reaches the heap
through, with a count. A function reaches the heap when it names an entry point, or when it
calls, within the archive, a function that does; that is iterated to a fixpoint, so a new
function that only calls an allocating helper (``vector::push_back`` into an already-pinned
``_M_realloc_insert``) is a site of its own and fails. The entry points:

* the C allocator: ``malloc``, ``calloc``, ``realloc``, ``free``, ``aligned_alloc``,
  ``posix_memalign``, ``memalign``, ``valloc``, ``pvalloc``, ``reallocarray``, ``strdup``,
  ``strndup``, and newlib's reentrant ``_malloc_r`` / ``_calloc_r`` / ``_realloc_r`` /
  ``_free_r`` / ``_memalign_r``;
* C++: every ``operator new``, ``operator new[]``, ``operator delete`` and
  ``operator delete[]``, in every variant (sized, aligned, nothrow);
* the ESP-IDF and FreeRTOS heaps: ``heap_caps_*alloc*`` / ``heap_caps_free``,
  ``pvPortMalloc``, ``vPortFree``;
* calls out of the archive that allocate without naming any of those: starting a
  ``std::thread``, ``pthread_create``, the FreeRTOS creators that are not ``...Static``
  (``xTaskCreate``, ``xQueueGenericCreate``, ...), and ``std::pmr::new_delete_resource``.

The limit: a call out of the archive into other code that allocates, and is not in that
list (newlib stdio, a libstdc++ member defined out of line), is not seen. The closure stops at
the archive's edge.

A deleting destructor counts: a class with a virtual destructor references
``operator delete`` from every object that emits its vtable, whether or not anything ever
deletes one, and the linker cannot tell the difference either.

* RATCHET. ``tools/no_heap_baseline.json`` pins, per target, every function that reaches the
  heap today: ``{object: {section: {entry point or "via <callee>": count}}}``, one line per
  function. A function the baseline does not list fails, and so does a pinned one that gained
  a path or a count: a heap call or an allocating helper called from a new function, or one
  more call to either from a function that already had one. A pinned site that is gone or shrank also
  fails until ``--repin`` lowers it, so the baseline never claims more than the truth.
  ``--repin`` only lowers pins; it never adds one.
* RENAME. ``--rebaseline`` re-seeds the pins after a rename or a signature change, and only
  that. Each function the baseline lacks must pair with a pinned one the build no longer has,
  in the SAME object and with the same demangled name once the parameter list and clone
  suffixes (``.isra.0``, ``.constprop.0``) are stripped. A pair may keep its keys and counts or
  have fewer, never a new key or a larger count; a ``via`` key may change only to the new name of
  a callee that is itself a renamed pair. A function both builds have follows the same rule, so
  its ``via`` keys can follow a renamed callee and nothing else. Per object the direct entry
  points may not exceed the pins. An unpaired new function, an unpaired vanished pin (use
  ``--repin``) or any rule broken refuses, lists every offender and writes nothing; success
  prints each pair that moved.
* ZERO is the end state: an empty target in the baseline, and then this check is the plain
  "no heap symbol at all" gate ADR-0083 §Consequences names. ``--strict`` runs that gate now.

A heap-rooted build (``kSlabPool`` false and ``kArenaBytes`` 0, ``CONFIG_LIBTRACER_DEFAULT_ROOT_HEAP``
on ESP-IDF, #2090) is the one deliberate exception: its default root IS the platform heap, so
``mem_heap`` reaches ``operator new`` and ``operator delete`` there by design. This check runs on
the arena-rooted archives only; it is not a gate for that image.

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
# Out-of-archive calls that allocate without naming an entry point above.
EXTERNAL = re.compile(r"^(?:_ZNSt6thread15_M_start_thread\w*|pthread_create|xTaskCreate|"
                      r"xTaskCreatePinnedToCore|xQueueGenericCreate|xEventGroupCreate|"
                      r"xTimerCreate|xStreamBufferGenericCreate|_ZNSt3pmr20new_delete_resourceEv)$")
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
    if HEAP_CAPS.match(symbol) or EXTERNAL.match(symbol):
        return symbol
    for prefix, name in CXX_HEAP.items():
        if symbol.startswith(prefix):
            return name
    return None


def short_section(section: str) -> str:
    """@brief A function section's symbol (``.text.<sym>`` -> ``<sym>``); others unchanged."""
    return section[len(".text."):] if section.startswith(".text.") else section


SYMBOL = re.compile(r"^[0-9a-fA-F]+ (.{7}) (\S+)\t[0-9a-fA-F]+ (.+)$")
METADATA = (".debug", ".eh_frame", ".ARM.ex", ".ARM.extab", ".rela", ".rel.")


class Archive:
    """@brief What ``objdump -r -t`` says about an archive: who defines what, who references what.

    * ``refs[(member, section)]``: {referenced symbol: count}, one count per relocation;
    * ``local[(member, name)]`` / ``exported[name]``: the section a symbol is defined in, a
      local one within its own member and a global or weak one in any member (an inline
      function emitted in several members is the same code, so any definition stands for it);
    * ``sections[member]``: the member's section names, so a relocation against a section
      symbol resolves to that section.
    """

    def __init__(self, text: str):
        self.refs: dict[tuple[str, str], dict[str, int]] = {}
        self.local: dict[tuple[str, str], str] = {}
        self.exported: dict[str, list[tuple[str, str]]] = {}
        self.sections: dict[str, set[str]] = {}
        member, section = None, None
        for line in text.splitlines():
            if "file format" in line:
                member = Path(line.split(":", 1)[0].strip()).name
                self.sections.setdefault(member, set())
                section = None
            elif member is None:
                continue
            elif line.startswith("RELOCATION RECORDS FOR ["):
                section = line[len("RELOCATION RECORDS FOR ["):].rstrip(":").rstrip("]")
                if section.startswith(METADATA):
                    section = None
            elif m := SYMBOL.match(line):
                flags, sec, name = m.group(1), m.group(2), m.group(3).strip()
                if sec in ("*UND*", "*ABS*", "*COM*"):
                    continue
                self.sections[member].add(sec)
                if "d" in flags:          # the section symbol itself
                    continue
                if flags[0] == "l":
                    self.local[(member, name)] = sec
                else:
                    self.exported.setdefault(name, []).append((member, sec))
            elif section is not None:
                parts = line.split()
                if len(parts) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
                    sym = parts[2].split("+")[0]
                    per = self.refs.setdefault((member, section), {})
                    per[sym] = per.get(sym, 0) + 1

    def resolve(self, member: str, sym: str) -> list[tuple[str, str]]:
        """@brief The (member, section)s a reference from @p member to @p sym can land in."""
        if sym in self.sections.get(member, ()):
            return [(member, sym)]
        if (member, sym) in self.local:
            return [(member, self.local[(member, sym)])]
        return self.exported.get(sym, [])


def is_code(section: str) -> bool:
    """@brief Whether @p section holds code, the only kind a call can reach the heap through."""
    return section == ".text" or section.startswith(".text.")


def heap_sites(arc: Archive) -> Sites:
    """@brief Every section that reaches the heap, with what it reaches it through.

    A section is a heap site when it names a heap entry point, or when it is code that calls,
    within the archive, code that is one: iterated to a fixpoint, so a new function that only
    calls an allocating helper (``vector::push_back`` into an already-pinned
    ``_M_realloc_insert``) is a site of its own. A site's keys are the heap entry points it
    names and ``via <symbol>`` for each heap-reaching callee, each with its count.
    """
    reach = {k for k, syms in arc.refs.items() if any(family(s) for s in syms)}
    changed = True
    while changed:
        changed = False
        for key, syms in arc.refs.items():
            if key in reach or not is_code(key[1]):
                continue
            if any(t in reach and is_code(t[1]) for s in syms for t in arc.resolve(key[0], s)):
                reach.add(key)
                changed = True
    sites: Sites = {}
    for member, section in reach:
        out: dict[str, int] = {}
        for sym, n in arc.refs[(member, section)].items():
            fam = family(sym)
            if fam is not None:
                out[fam] = out.get(fam, 0) + n
            elif is_code(section) and any(t in reach and is_code(t[1])
                                          for t in arc.resolve(member, sym)):
                k = "via " + short_section(sym)
                out[k] = out.get(k, 0) + n
        sites.setdefault(member, {})[short_section(section)] = out
    return sites


def parse_relocs(text: str) -> Sites:
    """@brief ``objdump -r -t`` output -> {object: {section: {entry point or via: count}}}."""
    return heap_sites(Archive(text))


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


CLONE = re.compile(r"\.(?:isra|constprop|part|cold|lto_priv)\.\d+")


def base_name(section: str, names: dict[str, str]) -> str:
    """@brief The demangled function name of @p section without parameters or clone suffixes."""
    text = re.sub(r" \[clone [^\]]*\]", "", CLONE.sub("", names.get(section, section)))
    if text.endswith(")") or text.endswith(") const"):
        text = text[:-6] if text.endswith(") const") else text
        depth = 0
        for i in range(len(text) - 1, -1, -1):
            depth += (text[i] == ")") - (text[i] == "(")
            if depth == 0:
                return text[:i]
    return text


def rebaseline(pinned: Sites, found: Sites, names: dict[str, str] | None = None):
    """@brief (problems, moves): why @p found is not @p pinned plus renames, and the pairs.

    @p problems is empty when the change is renames alone; @p moves lists (object, old
    section, new section) for each renamed pair.
    """
    names = names or {}
    f, p = flat_sites(found), flat_sites(pinned)
    gone = {k: p[k] for k in p.keys() - f.keys()}
    new = {k: f[k] for k in f.keys() - p.keys()}
    problems: list[str] = []
    pairs: list[tuple[tuple[str, str], tuple[str, str]]] = []
    groups: dict[tuple[str, str], tuple[list, list]] = {}
    for k in gone:
        groups.setdefault((k[0], base_name(k[1], names)), ([], []))[0].append(k)
    for k in new:
        groups.setdefault((k[0], base_name(k[1], names)), ([], []))[1].append(k)
    for (obj, base), (olds, news) in sorted(groups.items()):
        if not olds:
            problems.append(f"{obj} {base}: new function reaches the heap, with no pinned "
                            f"function of that name to be a rename of")
        elif not news:
            problems.append(f"{obj} {base}: pinned function is gone (lower it with --repin)")
        elif len(olds) != len(news):
            problems.append(f"{obj} {base}: {len(olds)} pinned against {len(news)} found; "
                            f"not a one-to-one rename")
        else:
            def shape(fams):
                return sorted((k if not k.startswith("via ") else "via", n)
                              for k, n in fams.items())
            olds, news = sorted(olds, key=lambda k: shape(gone[k])), \
                sorted(news, key=lambda k: shape(new[k]))
            pairs += list(zip(olds, news))
    rename = {o[1]: n[1] for o, n in pairs}
    rename = {short_section(o): short_section(n) for o, n in rename.items()}

    def follow(fams):
        return {("via " + rename.get(k[4:], k[4:]) if k.startswith("via ") else k): n
                for k, n in fams.items()}

    def check(label, was, now):
        allowed = follow(was)
        for fam, n in now.items():
            if n > allowed.get(fam, 0):
                problems.append(f"{label}: {fam} x{n} against {allowed.get(fam, 0)} pinned"
                                f"; that is a new heap path, not a rename")
    for old, nw in pairs:
        check(f"{nw[0]} {nw[1]}", p[old], f[nw])
    for key in p.keys() & f.keys():
        check(f"{key[0]} {key[1]}", p[key], f[key])
    for obj in {k[0] for k in f}:
        direct = lambda tab: sum(n for (o, _), fams in tab.items() if o == obj
                                 for k, n in fams.items() if not k.startswith("via "))
        if direct(f) > direct(p):
            problems.append(f"{obj}: direct heap calls grew {direct(p)} -> {direct(f)}")
    moves = [(o[0], o[1], n[1]) for o, n in pairs]
    return problems, moves


def flat_sites(sites: Sites) -> dict[tuple[str, str], dict[str, int]]:
    """@brief {(object, section): {entry point: count}}."""
    return {(o, s): fams for o, secs in sites.items() for s, fams in secs.items()}


def dump(data: dict) -> str:
    """@brief The baseline as JSON, one line per pinned function so a diff names the function."""
    comment = data.get("_comment", [])
    out = ["{", '  "_comment": [']
    out += [f"    {json.dumps(c)}" + ("," if i + 1 < len(comment) else "")
            for i, c in enumerate(comment)]
    out += ["  ],", '  "targets": {']
    targets = data.get("targets", {})
    for ti, (name, entry) in enumerate(sorted(targets.items())):
        out.append(f"    {json.dumps(name)}: {{")
        out.append(f'      "source": {json.dumps(entry.get("source", ""))},')
        out.append('      "sites": {')
        objs = sorted(entry.get("sites", {}).items())
        for oi, (obj, secs) in enumerate(objs):
            out.append(f"        {json.dumps(obj)}: {{")
            rows = sorted(secs.items())
            for si, (sec, fams) in enumerate(rows):
                comma = "," if si + 1 < len(rows) else ""
                out.append(f"          {json.dumps(sec)}: {json.dumps(fams, sort_keys=True)}{comma}")
            out.append("        }" + ("," if oi + 1 < len(objs) else ""))
        out.append("      }")
        out.append("    }" + ("," if ti + 1 < len(targets) else ""))
    out += ["  }", "}"]
    return "\n".join(out) + "\n"


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
    mode.add_argument("--rebaseline", action="store_true",
                      help="re-seed the pins after a pure rename; refuses anything else")
    mode.add_argument("--strict", action="store_true", help="ignore the baseline: zero or fail")
    mode.add_argument("--seed", metavar="SOURCE",
                      help="pin every site for a target the baseline lacks; SOURCE says what "
                           "build the pins came from")
    args = ap.parse_args()

    try:
        res = subprocess.run([args.objdump, "-r", "-t", args.archive], capture_output=True, text=True)
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
        path.write_text(dump(data), encoding="utf-8")
        print(f"seeded {args.target}: {sum(flatten(found).values())} reference(s) from "
              f"{sum(len(v) for v in found.values())} function(s)")
        return 0
    if entry is None and not args.strict:
        print(f"::error::no-heap: target {args.target!r} has no entry in {args.baseline}")
        return 1
    if args.rebaseline:
        raw = sorted({k[1] for k in flat_sites(found)} | {k[1] for k in flat_sites(
            entry.get("sites", {}))})
        problems, moves = rebaseline(entry.get("sites", {}), found, demangle(raw))
        if problems:
            for why in problems:
                print(f"::error::no-heap [{args.target}]: --rebaseline refused: {why}")
            return 1
        names = demangle(sorted({m[2] for m in moves} | {m[1] for m in moves}))
        for obj, old, nw in moves:
            print(f"moved: {obj}: {names.get(old, old)} -> {names.get(nw, nw)}")
        entry["sites"] = found
        path.write_text(dump(data), encoding="utf-8")
        print(f"rebaselined {args.target}: {len(moves)} rename(s), "
              f"{sum(flatten(found).values())} reference(s) re-seeded")
        return 0
    pinned: Sites = {} if args.strict else entry.get("sites", {})
    new, gone = compare(found, pinned)

    flat = flatten(found)
    print(f"no-heap [{args.target}]: {sum(flat.values())} heap reference(s) from "
          f"{sum(len(v) for v in found.values())} function(s) in {len(found)} object(s) of "
          f"{args.archive}; "
          f"{sum(flatten(pinned).values())} pinned.")

    if args.repin and not new:
        if gone:
            entry["sites"] = lower(pinned, found)
            path.write_text(dump(data), encoding="utf-8")
            print(f"repinned: lowered {len(gone)} site(s)")
        return 0

    names = demangle(sorted({s for (_, s, _) in list(new) + list(gone)} |
                            {f[4:] for (_, _, f) in list(new) + list(gone) if f.startswith("via ")}))
    for (obj, sec, fam), (was, now) in sorted(new.items()):
        what = f"calls {names.get(fam[4:], fam[4:])}, which reaches the heap" \
            if fam.startswith("via ") else f"calls {fam}"
        print(f"::error::no-heap [{args.target}]: {obj} {names.get(sec, sec)} {what} "
              f"({now} reference(s), {was} pinned); libtracer.a must not reach the heap "
              f"(ADR-0083). Draw from the injected block source.")
    for (obj, sec, fam), (was, now) in sorted(gone.items()):
        print(f"::error::no-heap [{args.target}]: {obj} {names.get(sec, sec)} references "
              f"{names.get(fam[4:], fam) if fam.startswith('via ') else fam} {now} time(s), "
              f"{was} pinned. Lower the pin: rerun with --repin.")
    if new or gone:
        return 1
    print("no-heap: OK — nothing beyond the pinned baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
