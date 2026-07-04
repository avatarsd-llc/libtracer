#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Cortex-M0 required-modules footprint sentinel (ADR-0047 §5).

Cross-compiles the minimum-feature (P0) libtracer node — the "required modules"
a bare-metal MCU links, exercised by the fixed fixture
`core/tests/footprint/sentinel_node.cpp` — for arm-none-eabi Cortex-M0 with the
MCU profile (`-std=c++23 -Os -fno-exceptions -fno-rtti`, `LIBTRACER_NO_ATOMIC`,
`-ffunction-sections -fdata-sections` + `--gc-sections`), links and sizes it,
and gates the flash footprint (text + rodata + data-initializers) at a hard
ceiling. RAM occupancy (data + bss) is reported but not gated — the gate is the
flash budget the doctrine names.

The doctrine (ADR-0047 §1, §5): compile-time / template-metaprogramming
techniques are admissible exactly as far as this gate stays green — the same
codebase serves ESP32-class MCUs and 128-core hosts, and this referee cuts both
ways, catching vtable/erasure bloat AND template-instantiation bloat. When the
module-set work (Waves 1-3) lands, the delta on this number is the evidence.

Gate modes (mirroring tools/esp_size_gate.py):
  * warn — over budget prints `::warning::` and exits 0. The default *today*:
    the measured full send+receive P0 node is ~0.9 KiB over the 16 KiB bound
    (`std::pmr` drags ~2.7 KiB of soft-float via the ADR-0041 arena decoder's
    `memory_resource&` interface; ~1.5 KiB more is CRC lookup tables), so a hard
    gate would red main. Per the PR #193 precedent, no over-budget number reds
    main until the required modules are genuinely under the ceiling.
  * fail — over budget prints `::error::` and exits 1. Flip the workflow to this
    once the Wave 2 arena/codec rework (narrowing the decoder's allocator seam
    off full `std::pmr`, dropping the vector-tree codec) brings the required
    modules under 16 KiB — then the doctrine's hard gate (ADR-0047 §5) is in
    force and this referee catches any regression.

A toolchain-missing or size-parse failure is an `::error::` + exit 1 in fail
mode (a broken gate must never masquerade as green) and a warning + exit 0 in
warn mode. Stdlib only.
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

# The required-modules set: the L0/L1 substrate (bounded pool + segment/view/
# rope), the L2/L3 wire codec (frame + terminus arena decode), and L4 addressing
# (path). No graph runtime, no transports, no threads — the surface v1.md §3.1
# guarantees an MCU can carry. Keep in sync with sentinel_node.cpp's includes.
REQUIRED_MODULES = ("frame", "tlv_arena", "mem_pool", "rope", "path")
FIXTURE = "core/tests/footprint/sentinel_node.cpp"
DEFAULT_BUDGET = 16 * 1024  # 16 KiB — the ADR-0016 §3 / ADR-0047 §5 Cortex-M0 bound.


def _fmt(n: int | None) -> str:
    """Human-friendly byte count for the markdown table."""
    if n is None:
        return "n/a"
    return f"{n:,} B ({n / 1024:.2f} KiB)"


def _repo_root() -> pathlib.Path:
    """The repository root (this script lives in <root>/tools/)."""
    return pathlib.Path(__file__).resolve().parent.parent


def _run(cmd: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def _compile_and_link(
    cxx: str, mcpu: str, root: pathlib.Path, workdir: pathlib.Path
) -> pathlib.Path:
    """Compile the required modules + fixture and link the stripped image.

    Raises RuntimeError with the compiler diagnostic on any failure.
    """
    include = root / "core" / "include"
    cxx_flags = [
        "-std=c++23",
        "-Os",
        "-fno-exceptions",
        "-fno-rtti",
        f"-mcpu={mcpu}",
        "-mthumb",
        "-DLIBTRACER_NO_ATOMIC",
        "-ffunction-sections",
        "-fdata-sections",
        "-Wall",
        "-Wextra",
        f"-I{include}",
    ]
    link_flags = [
        "-Os",
        f"-mcpu={mcpu}",
        "-mthumb",
        "--specs=nano.specs",
        "--specs=nosys.specs",
        "-Wl,--gc-sections",
        "-s",  # strip: the sentinel measures the shipped image, not debug info.
    ]

    objects: list[str] = []
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

    elf = workdir / "sentinel.elf"
    res = _run([cxx, *link_flags, *objects, "-o", str(elf)], cwd=root)
    if res.returncode != 0:
        raise RuntimeError(f"link failed:\n{res.stderr.strip()}")
    return elf


def _measure(size_tool: str, elf: pathlib.Path, root: pathlib.Path) -> tuple[int, int, int]:
    """Return (text, data, bss) from `<size> <elf>` (Berkeley format).

    For arm-none-eabi bare-metal, `.rodata` folds into the `text` output section,
    so text already covers code + read-only data; the initialized-data image
    (`data`) also lives in flash. Flash footprint = text + data; RAM = data + bss.
    """
    res = _run([size_tool, str(elf)], cwd=root)
    if res.returncode != 0:
        raise RuntimeError(f"size tool failed:\n{res.stderr.strip()}")
    lines = [ln for ln in res.stdout.splitlines() if ln.strip()]
    if len(lines) < 2:
        raise RuntimeError(f"unexpected size output:\n{res.stdout}")
    fields = lines[1].split()
    try:
        text, data, bss = int(fields[0]), int(fields[1]), int(fields[2])
    except (IndexError, ValueError) as exc:
        raise RuntimeError(f"could not parse size row {lines[1]!r}: {exc}") from exc
    return text, data, bss


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cxx", default=os.environ.get("LIBTRACER_ARM_CXX", "arm-none-eabi-g++"),
                    help="C++ cross-compiler (default: arm-none-eabi-g++ or $LIBTRACER_ARM_CXX)")
    ap.add_argument("--size", default=os.environ.get("LIBTRACER_ARM_SIZE", "arm-none-eabi-size"),
                    help="the `size` tool (default: arm-none-eabi-size or $LIBTRACER_ARM_SIZE)")
    ap.add_argument("--mcpu", default="cortex-m0", help="target core (default: cortex-m0)")
    ap.add_argument("--max-flash-bytes", type=int, default=DEFAULT_BUDGET,
                    help=f"hard flash budget (default: {DEFAULT_BUDGET} = 16 KiB)")
    ap.add_argument("--mode", choices=("warn", "fail"), default="warn")
    ap.add_argument("--out-json", default=None, help="write the numbers as a JSON artifact")
    args = ap.parse_args()

    root = _repo_root()
    cxx = shutil.which(args.cxx) or args.cxx
    size_tool = shutil.which(args.size) or args.size

    def _fail(msg: str) -> int:
        if args.mode == "fail":
            print(f"::error::{msg}")
            return 1
        print(f"::warning::{msg} (warn mode: not failing the job)")
        return 0

    if not (shutil.which(args.cxx) or pathlib.Path(cxx).is_file()):
        return _fail(f"Cortex-M0 sentinel: cross-compiler {args.cxx!r} not found on PATH")

    try:
        with tempfile.TemporaryDirectory(prefix="cortexm0_sentinel_") as tmp:
            workdir = pathlib.Path(tmp)
            elf = _compile_and_link(cxx, args.mcpu, root, workdir)
            text, data, bss = _measure(size_tool, elf, root)
    except RuntimeError as exc:
        return _fail(f"Cortex-M0 sentinel could not build/measure the required modules: {exc}")

    flash = text + data
    ram = data + bss
    flash_ok = flash <= args.max_flash_bytes
    headroom = args.max_flash_bytes - flash
    verdict = "PASS" if flash_ok else ("WARN" if args.mode == "warn" else "FAIL")
    modules = ", ".join(f"`{m}`" for m in REQUIRED_MODULES)

    lines = [
        f"## libtracer Cortex-M0 footprint — required modules ({args.mcpu})",
        "",
        f"Profile: `arm-none-eabi-g++ -std=c++23 -Os -fno-exceptions -fno-rtti "
        f"-DLIBTRACER_NO_ATOMIC` + `--gc-sections`, stripped.",
        f"Required modules: {modules} + the `sentinel_node` fixture.",
        "",
        "| Section | Bytes |",
        "| --- | --- |",
        f"| text (code + .rodata) | {_fmt(text)} |",
        f"| data (initialized RAM, flash-backed) | {_fmt(data)} |",
        f"| bss (zero RAM) | {_fmt(bss)} |",
        f"| **flash footprint** (text + data) | **{_fmt(flash)}** |",
        f"| RAM occupancy (data + bss) | {_fmt(ram)} |",
        f"| flash budget (mode={args.mode}) | {_fmt(args.max_flash_bytes)} |",
        "",
        f"**Sentinel: {verdict}** — flash {_fmt(flash)} vs budget {_fmt(args.max_flash_bytes)} "
        + (f"({_fmt(headroom)} headroom)." if flash_ok
           else f"(**over by {_fmt(-headroom)}**)."),
        "",
    ]
    report = "\n".join(lines)
    print(report)
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as f:
            f.write(report + "\n")

    if args.out_json:
        payload = {
            "mcpu": args.mcpu,
            "required_modules": list(REQUIRED_MODULES),
            "text_bytes": text,
            "data_bytes": data,
            "bss_bytes": bss,
            "flash_bytes": flash,
            "ram_bytes": ram,
            "max_flash_bytes": args.max_flash_bytes,
            "headroom_bytes": headroom,
            "mode": args.mode,
            "verdict": verdict,
        }
        pathlib.Path(args.out_json).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    if flash_ok:
        return 0
    over = (
        f"Cortex-M0 required-modules flash footprint {_fmt(flash)} exceeds the "
        f"{_fmt(args.max_flash_bytes)} budget by {_fmt(-headroom)} (ADR-0047 §5)"
    )
    if args.mode == "fail":
        print(f"::error::{over}")
        return 1
    print(f"::warning::{over} (warn mode: not failing the job — tighten or investigate)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
