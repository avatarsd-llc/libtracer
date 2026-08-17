#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""rv32 (ESP32-C3/C6 class) required-modules flash census.

The NARROW-end companion to `tools/cortexm0_footprint.py`. Same fixture, same
`REQUIRED_MODULES` set, same `-Os` MCU profile — a different target triple, and
a different job. The Cortex-M0 script is a *gate* (it carries a 16 KiB budget
and a drift comparison); this one is a *census*: it reports where the flash of
the minimum-feature (P0) node sits, broken out per output section
(`.text` / `.rodata` / unwind / `.data`) and per translation unit, so a delta
between two commits can be **attributed** instead of only totalled.

Why a separate instrument rather than a `--mcpu` on the M0 gate: the M0 gate's
flag set is Thumb-specific (`-mthumb`, `--specs=nano.specs` against ARM newlib)
and its Berkeley-format `size` parse folds `.rodata` into `text`, which is
exactly the split a census has to preserve. rv32 is also the target that
actually ships (ADR-0001: the originating firmware is an ESP32-C6 node), so the
number it produces answers a different question from the M0 doctrine referee.

**Attribution, not a total.** A whole-image byte count cannot discharge a size
falsifier — "the image is N bytes" says nothing about what a given change cost.
Run this at two commits with the SAME toolchain and diff: `--baseline-json`
prints the per-section and per-object deltas.

**Toolchain.** Defaults to `riscv32-esp-elf-g++` on `PATH`. The tree's floor is
GCC 15 (IDF v6.0 pins `esp-15.2.0_20251204`); a number from an older crosstool
is not comparable to one from a newer, so every published figure carries the
compiler's own `--version` line, as on the M0 gate (#1138: a ~1 KiB
cross-toolchain spread on this same fixture once manufactured a phantom
regression).

**A failure to MEASURE is an error, never a quiet zero.** A missing toolchain,
a compile error, a link error or an unparsable `size` table exits 1 — the M0
gate reported success while measuring nothing twice (#982) and this instrument
inherits that lesson rather than repeating it. Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

# Keep in sync with tools/cortexm0_footprint.py's REQUIRED_MODULES and with
# sentinel_node.cpp's includes: the L0/L1 substrate, the L2/L3 wire codec and
# L4 addressing — the surface v1.md §3.1 guarantees an MCU can carry.
REQUIRED_MODULES = ("frame", "tlv_arena", "backend_set", "mem_pool", "mem_source", "rope", "path")
FIXTURE = "core/tests/footprint/sentinel_node.cpp"

# ESP32-C6 (rv32imac) is the reference core of ADR-0001's originating firmware;
# `--march rv32imc_zicsr_zifencei` selects the ESP32-C3 variant.
DEFAULT_MARCH = "rv32imac_zicsr_zifencei"
DEFAULT_MABI = "ilp32"


def _fmt(n: int) -> str:
    """Human-friendly byte count for the markdown table."""
    return f"{n:,} B ({n / 1024:.2f} KiB)"


def _repo_root() -> pathlib.Path:
    """The repository root (this script lives in <root>/tools/)."""
    return pathlib.Path(__file__).resolve().parent.parent


def _run(cmd: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def _toolchain_id(cxx: str, root: pathlib.Path) -> str:
    """The compiler's own identity line, which labels every published number."""
    try:
        res = _run([cxx, "--version"], cwd=root)
    except OSError:
        return "unknown"
    if res.returncode != 0 or not res.stdout.strip():
        return "unknown"
    return res.stdout.splitlines()[0].strip()


def _sections(size_tool: str, obj: pathlib.Path, root: pathlib.Path) -> dict[str, int]:
    """Return {section name: bytes} from `size -A` (sysv format).

    Berkeley format folds `.rodata` into `text`; the census needs the two apart,
    so it reads the sysv table instead.
    """
    res = _run([size_tool, "-A", str(obj)], cwd=root)
    if res.returncode != 0:
        raise RuntimeError(f"size tool failed on {obj.name}:\n{res.stderr.strip()}")
    out: dict[str, int] = {}
    for line in res.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith("."):
            try:
                out[fields[0]] = int(fields[1])
            except ValueError:
                continue
    if not out:
        raise RuntimeError(f"could not parse `size -A` output for {obj.name}:\n{res.stdout}")
    return out


def _roll_up(sec: dict[str, int]) -> dict[str, int]:
    """Collapse the sysv section table into the census buckets.

    `.srodata` / `.sdata` / `.sbss` are the RISC-V gp-relative small-data
    sections; they belong with their large counterparts. Unwind tables
    (`.eh_frame`, `.gcc_except_table`) occupy flash but arrive from newlib
    rather than from libtracer under `-fno-exceptions`, so they are bucketed
    separately — folding them into `.text` would attribute libc to the change
    under test.
    """

    def total(*prefixes: str) -> int:
        return sum(v for k, v in sec.items() if any(k.startswith(p) for p in prefixes))

    text = total(".text") + sum(v for k, v in sec.items() if k in (".init", ".fini"))
    rodata = total(".rodata", ".srodata")
    unwind = sum(v for k, v in sec.items() if k in (".eh_frame", ".gcc_except_table"))
    tables = sum(v for k, v in sec.items()
                 if k in (".init_array", ".fini_array", ".preinit_array"))
    data = total(".data", ".sdata")
    bss = total(".bss", ".sbss")
    return {
        "text": text,
        "rodata": rodata,
        "unwind": unwind,
        "init_arrays": tables,
        "data": data,
        "bss": bss,
        "flash": text + rodata + unwind + tables + data,
        "ram": data + bss,
    }


def _census(cxx: str, size_tool: str, march: str, mabi: str, root: pathlib.Path) -> dict:
    """Compile, link and size the sentinel; return the census payload.

    Raises RuntimeError with the compiler diagnostic on any failure.
    """
    cxx_flags = [
        "-std=c++23",
        "-Os",
        "-fno-exceptions",
        "-fno-rtti",
        f"-march={march}",
        f"-mabi={mabi}",
        "-DLIBTRACER_NO_ATOMIC",
        "-DNDEBUG",
        "-DLIBTRACER_BACKEND_SET_POOL_ONLY",
        "-ffunction-sections",
        "-fdata-sections",
        "-Wall",
        "-Wextra",
        f"-I{root / 'core' / 'include'}",
    ]
    link_flags = [
        "-Os",
        f"-march={march}",
        f"-mabi={mabi}",
        "--specs=nano.specs",
        "--specs=nosys.specs",
        "-Wl,--gc-sections",
        "-s",
    ]

    with tempfile.TemporaryDirectory(prefix="rv32_census_") as tmp:
        workdir = pathlib.Path(tmp)
        objects: list[str] = []
        per_object: dict[str, dict[str, int]] = {}
        units = [root / "core" / "src" / f"{m}.cpp" for m in REQUIRED_MODULES]
        units.append(root / FIXTURE)
        for src in units:
            if not src.is_file():
                raise RuntimeError(f"source not found: {src}")
            obj = workdir / (src.stem + ".o")
            res = _run([cxx, *cxx_flags, "-c", str(src), "-o", str(obj)], cwd=root)
            if res.returncode != 0:
                raise RuntimeError(f"compile failed for {src.name}:\n{res.stderr.strip()}")
            objects.append(str(obj))
            per_object[src.stem] = _roll_up(_sections(size_tool, obj, root))

        elf = workdir / "sentinel.elf"
        res = _run([cxx, *link_flags, *objects, "-o", str(elf)], cwd=root)
        if res.returncode != 0:
            raise RuntimeError(f"link failed:\n{res.stderr.strip()}")
        linked_sections = _sections(size_tool, elf, root)

    payload = _roll_up(linked_sections)
    payload["sections"] = linked_sections
    payload["per_object"] = per_object
    return payload


def _delta_table(title: str, base: dict, cur: dict, keys: tuple[str, ...]) -> list[str]:
    """Markdown rows for a baseline-vs-current comparison."""
    lines = [f"### {title}", "", "| bucket | baseline | current | delta |", "| --- | --- | --- | --- |"]
    for k in keys:
        lines.append(f"| {k} | {base[k]:,} B | {cur[k]:,} B | **{cur[k] - base[k]:+,} B** |")
    lines.append("")
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cxx", default=os.environ.get("LIBTRACER_RV32_CXX", "riscv32-esp-elf-g++"),
                    help="C++ cross-compiler (default: riscv32-esp-elf-g++ or $LIBTRACER_RV32_CXX)")
    ap.add_argument("--size", default=os.environ.get("LIBTRACER_RV32_SIZE", "riscv32-esp-elf-size"),
                    help="the `size` tool (default: riscv32-esp-elf-size or $LIBTRACER_RV32_SIZE)")
    ap.add_argument("--march", default=DEFAULT_MARCH,
                    help=f"RISC-V ISA string (default: {DEFAULT_MARCH}, the ESP32-C6 core)")
    ap.add_argument("--mabi", default=DEFAULT_MABI, help=f"ABI (default: {DEFAULT_MABI})")
    ap.add_argument("--baseline-json", default=None,
                    help="a previous --out-json payload; prints per-section and per-object deltas")
    ap.add_argument("--out-json", default=None, help="write the numbers as a JSON artifact")
    args = ap.parse_args()

    root = _repo_root()
    cxx = shutil.which(args.cxx) or args.cxx
    size_tool = shutil.which(args.size) or args.size

    def _broken(msg: str) -> int:
        """A census that cannot measure is BROKEN — never a quiet zero (#982)."""
        print(f"::error::{msg}")
        return 1

    if not (shutil.which(args.cxx) or pathlib.Path(cxx).is_file()):
        return _broken(f"rv32 census: cross-compiler {args.cxx!r} not found on PATH")

    toolchain = _toolchain_id(cxx, root)

    try:
        payload = _census(cxx, size_tool, args.march, args.mabi, root)
    except RuntimeError as exc:
        return _broken(f"rv32 census could not build/measure the required modules: {exc}")

    payload.update({
        "march": args.march,
        "mabi": args.mabi,
        "toolchain": toolchain,
        "commit": os.environ.get("GITHUB_SHA"),
        "required_modules": list(REQUIRED_MODULES),
    })

    buckets = ("text", "rodata", "unwind", "init_arrays", "data", "flash", "bss", "ram")
    modules = ", ".join(f"`{m}`" for m in REQUIRED_MODULES)
    lines = [
        f"## libtracer rv32 flash census — required modules (`{args.march}`/`{args.mabi}`)",
        "",
        "Profile: `-std=c++23 -Os -fno-exceptions -fno-rtti -DLIBTRACER_NO_ATOMIC "
        "-DNDEBUG -DLIBTRACER_BACKEND_SET_POOL_ONLY` + `--gc-sections`, stripped.",
        f"Required modules: {modules} + the `sentinel_node` fixture.",
        f"Toolchain: `{toolchain}` — compare numbers ONLY within one toolchain.",
        "",
        "| bucket | bytes |",
        "| --- | --- |",
        f"| `.text` | {_fmt(payload['text'])} |",
        f"| `.rodata` (+ `.srodata`) | {_fmt(payload['rodata'])} |",
        f"| unwind (`.eh_frame`, `.gcc_except_table`; from newlib) | {_fmt(payload['unwind'])} |",
        f"| ctor/dtor tables | {_fmt(payload['init_arrays'])} |",
        f"| `.data` (initialized RAM, flash-backed) | {_fmt(payload['data'])} |",
        f"| **total flash** | **{_fmt(payload['flash'])}** |",
        f"| `.bss` (zero RAM) | {_fmt(payload['bss'])} |",
        f"| RAM occupancy (`.data` + `.bss`) | {_fmt(payload['ram'])} |",
        "",
        "| translation unit | `.text` | `.rodata` |",
        "| --- | --- | --- |",
    ]
    for name, obj in payload["per_object"].items():
        lines.append(f"| `{name}` | {obj['text']:,} B | {obj['rodata']:,} B |")
    lines.append("")

    rc = 0
    if args.baseline_json:
        base_path = pathlib.Path(args.baseline_json)
        if not base_path.is_file():
            return _broken(f"rv32 census: baseline {args.baseline_json!r} not present")
        try:
            base = json.loads(base_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            return _broken(f"rv32 census: baseline {args.baseline_json!r} unreadable: {exc}")
        base_tc = base.get("toolchain", "unknown")
        if base_tc != toolchain or toolchain == "unknown":
            # A cross-toolchain delta is precisely the invalid comparison the M0
            # gate learned to refuse (#1138); refusing it is not a measurement
            # failure, so this is a notice and rc stays 0.
            print(f"::notice::rv32 census: baseline comparison skipped — toolchain changed "
                  f"({base_tc!r} -> {toolchain!r})")
        else:
            lines += _delta_table(
                f"Delta vs baseline `{(base.get('commit') or 'unknown')[:12]}`",
                base, payload, buckets)
            lines += ["### Per-translation-unit delta", "",
                      "| translation unit | `.text` delta | `.rodata` delta |",
                      "| --- | --- | --- |"]
            for name, obj in payload["per_object"].items():
                prev = base.get("per_object", {}).get(name)
                if prev is None:
                    lines.append(f"| `{name}` | n/a (new) | n/a (new) |")
                    continue
                lines.append(f"| `{name}` | {obj['text'] - prev['text']:+,} B "
                             f"| {obj['rodata'] - prev['rodata']:+,} B |")
            lines.append("")

    report = "\n".join(lines)
    print(report)
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as f:
            f.write(report + "\n")

    if args.out_json:
        pathlib.Path(args.out_json).write_text(json.dumps(payload, indent=2) + "\n",
                                               encoding="utf-8")
    return rc


if __name__ == "__main__":
    sys.exit(main())
