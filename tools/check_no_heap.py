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
function that only calls an allocating helper (``vector::push_back`` into
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
  (``xTaskCreate``, ``xQueueGenericCreate``, and ``xQueueCreateMutex`` /
  ``xQueueCreateCountingSemaphore``, which ``xSemaphoreCreateMutex``,
  ``xSemaphoreCreateRecursiveMutex`` and ``xSemaphoreCreateCounting`` expand to, ...), and
  ``std::pmr::new_delete_resource``;
* the pthread synchronization objects (#2454): every ``pthread_mutex_*``, ``pthread_cond_*``
  and ``pthread_rwlock_*`` call, every out-of-line ``std::condition_variable`` /
  ``std::condition_variable_any`` member, and ``std::notify_all_at_thread_exit``. ESP-IDF's pthread layer gives each mutex, condition
  variable and rwlock a record from the heap on its init (and a mutex its FreeRTOS semaphore
  too), inside IDF's code, so no allocator
  symbol ever shows in the archive. A ``std::mutex`` lowers to exactly these calls; core
  spells ``tr::mutex_t`` / ``tr::condvar_t`` / ``tr::shared_mutex_t`` instead, which a chip
  binds to ``tr::esp::static_sync_t``. On the cortex-m0 archive (no threads) none appears.

The limit: a call out of the archive into other code that allocates, and is not in that
list (newlib stdio, a libstdc++ member defined out of line), is not seen. The closure stops at
the archive's edge.

A deleting destructor counts: a class with a virtual destructor references
``operator delete`` from every object that emits its vtable, whether or not anything ever
deletes one, and the linker cannot tell the difference either.

The gate is ZERO (#2288, ADR-0083 §Consequences): any reference fails, each one named by
object, function and entry point, and there is no baseline that could absorb one. The way out
of a failure is to draw from the injected block source, never to pin the call.

A heap-rooted build (``kSlabPool`` false and ``kArenaBytes`` 0, ``CONFIG_LIBTRACER_DEFAULT_ROOT_HEAP``
on ESP-IDF, #2090) is the one deliberate exception: its default root IS the platform heap, so
``mem_heap`` reaches ``operator new`` and ``operator delete`` there by design. This check runs on
the arena-rooted archives only; it is not a gate for that image.

CI gates two archives: ``cortex-m0`` (the required-module archive
``tools/cortexm0_footprint.py --archive-out`` writes) and ``esp32c6`` (the ESP-IDF component
archive of ``integrations/esp-idf/examples/full_node``). ``--target`` only labels the messages.

Usage::

    python3 tools/check_no_heap.py --target cortex-m0 --archive libtracer.a \\
        --objdump arm-none-eabi-objdump

Stdlib only; ``c++filt``, when present, demangles the section names in messages.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

C_HEAP = (
    "malloc", "calloc", "realloc", "free", "aligned_alloc", "posix_memalign", "memalign",
    "valloc", "pvalloc", "reallocarray", "strdup", "strndup",
    "_malloc_r", "_calloc_r", "_realloc_r", "_free_r", "_memalign_r",
)
RTOS_HEAP = ("pvPortMalloc", "vPortFree")
# Out-of-archive calls that allocate without naming an entry point above.
EXTERNAL = re.compile(r"^(?:_ZNSt6thread15_M_start_thread\w*|pthread_create|xTaskCreate|"
                      r"xTaskCreatePinnedToCore|xQueueGenericCreate|xEventGroupCreate|"
                      r"xTimerCreate|xStreamBufferGenericCreate|xQueueCreateMutex|"
                      r"xQueueCreateCountingSemaphore|_ZNSt3pmr20new_delete_resourceEv|"
                      r"_ZSt25notify_all_at_thread_exit\w*|"
                      r"pthread_(?:mutex|cond|rwlock)_\w+|"
                      r"_ZNSt(?:18condition_variable|22condition_variable_any)\w*)$")
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
    calls an allocating helper (``vector::push_back`` into
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


def demangle(names: list[str]) -> dict[str, str]:
    """@brief Best-effort ``c++filt`` over @p names; a name it cannot read maps to itself."""
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True,
                             text=True).stdout.splitlines()
    except OSError:
        return {n: n for n in names}
    return dict(zip(names, out)) if len(out) == len(names) else {n: n for n in names}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", required=True, help="the target the archive is for, for messages")
    ap.add_argument("--archive", required=True, help="the libtracer.a to read")
    ap.add_argument("--objdump", default="objdump", help="the target's objdump")
    args = ap.parse_args(argv)

    try:
        res = subprocess.run([args.objdump, "-r", "-t", args.archive], capture_output=True, text=True)
    except OSError as exc:
        print(f"::error::cannot run {args.objdump}: {exc}")
        return 1
    if res.returncode != 0:
        print(f"::error::{args.objdump} failed on {args.archive}:\n{res.stderr.strip()}")
        return 1
    found = parse_relocs(res.stdout)
    rows = sorted((o, s, f, n) for o, secs in found.items() for s, fams in secs.items()
                  for f, n in fams.items())
    print(f"no-heap [{args.target}]: {sum(r[3] for r in rows)} heap reference(s) from "
          f"{sum(len(v) for v in found.values())} function(s) in {len(found)} object(s) of "
          f"{args.archive}.")
    names = demangle(sorted({r[1] for r in rows} | {r[2][4:] for r in rows
                                                    if r[2].startswith("via ")}))
    for obj, sec, fam, n in rows:
        what = f"calls {names.get(fam[4:], fam[4:])}, which reaches the heap" \
            if fam.startswith("via ") else f"calls {fam}"
        print(f"::error::no-heap [{args.target}]: {obj} {names.get(sec, sec)} {what} "
              f"({n} reference(s)); libtracer.a must not reach the heap (ADR-0083). "
              f"Draw from the injected block source.")
    if rows:
        return 1
    print("no-heap: OK, libtracer.a reaches no heap entry point.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
