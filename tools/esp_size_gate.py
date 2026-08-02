#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Footprint reporter for the ESP-IDF full-node build (esp-idf.yml, full-node job).

Reads the per-archive and whole-image size JSON emitted by esp-idf-size for the
`full_node` example, extracts the libtracer component's contribution (flash =
text+rodata, static RAM = data+bss+iram), writes a markdown table to the GitHub
Actions step summary and dumps a machine-readable JSON artifact.

The footprint is TRACKED, not GATED. No ceiling is set in CI by maintainer
ruling: the library must stay free to serve the thinnest possible client, and
what has to fit is the product image on its own target — a number CI cannot
know. Regressions are caught by reading the tracked numbers, not by a
threshold. `--max-flash-bytes` / `--max-static-ram-bytes` are therefore both
optional and both default to "tracked, no ceiling"; a downstream consumer with
a real budget can pass one to turn this into a gate for its own build.

Accepts BOTH esp-idf-size output generations, so the workflow can fall back:
  * NG (esp-idf-size >= 2.0, IDF v6.0):  `--format json2 [--archives]`
  * legacy (IDF v5.x):                   `--format json [--archives]`

A parse failure ALWAYS prints `::error::` and exits 1 — a silently broken
reporter must not masquerade as a green report.

Exit 0 = report published (and within any ceiling that was passed), 1 = broken
input, or over a ceiling the caller opted into. Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sys
from typing import Any


def _fmt(n: int | None) -> str:
    """Human-friendly byte count for the markdown table."""
    if n is None:
        return "n/a"
    return f"{n:,} B ({n / 1024:.1f} KiB)"


def _load_json(path: str) -> Any:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ── component (per-archive) extraction ──────────────────────────────────────


def find_component(archives: dict[str, Any], pattern: str) -> tuple[str, Any]:
    """Locate the component's archive entry by substring/regex on the archive name."""
    rx = re.compile(pattern, re.IGNORECASE)
    matches = {name: info for name, info in archives.items() if rx.search(name)}
    if not matches:
        raise KeyError(f"no archive matching {pattern!r} in {sorted(archives)}")
    if len(matches) > 1:
        raise KeyError(f"archive pattern {pattern!r} is ambiguous: {sorted(matches)}")
    return next(iter(matches.items()))


def component_sizes(info: Any) -> tuple[int, int, dict[str, int]]:
    """Return (flash_bytes, static_ram_bytes, per-memory-type breakdown).

    NG shape:     {"abbrev_name": ..., "size": N, "memory_types":
                   {"Flash Code": {"size": N, "sections": {...}}, "DRAM": ...}}
    legacy shape: {"flash_text": N, "flash_rodata": N, "iram": N, "dram": N,
                   "diram": N, "ram_st_total": N, "flash_total": N, ...}
    """
    if isinstance(info, dict) and "memory_types" in info:  # NG (json2)
        flash = ram = 0
        breakdown: dict[str, int] = {}
        for mt_name, mt_info in info["memory_types"].items():
            size = int(mt_info.get("size", 0))
            if size == 0:
                continue
            breakdown[mt_name] = size
            if "flash" in mt_name.lower():
                flash += size
            else:  # DRAM / IRAM / DIRAM / RTC RAM — static RAM occupancy
                ram += size
        return flash, ram, breakdown
    if isinstance(info, dict) and ("flash_total" in info or "ram_st_total" in info):  # legacy
        flash = int(info.get("flash_total", 0))
        ram = int(info.get("ram_st_total", 0))
        breakdown = {
            k: int(v)
            for k, v in info.items()
            if isinstance(v, (int, float)) and v and k not in ("size", "size_diff")
        }
        return flash, ram, breakdown
    raise ValueError(f"unrecognized archive-entry shape: keys={sorted(info)}")


# ── whole-image totals ───────────────────────────────────────────────────────


def app_totals(summary: Any) -> tuple[int | None, dict[str, int]]:
    """Return (total_image_size, {memory type: used bytes}) from the summary JSON.

    NG shape:     {"version": "1.x", "total_size": N,
                   "layout": [{"name": ..., "total": ..., "used": ...}, ...]}
    legacy shape: {"total_size": N, "used_dram": N, "used_iram": N, ...}
    """
    if not isinstance(summary, dict):
        raise ValueError("summary JSON is not an object")
    total = summary.get("total_size")
    used: dict[str, int] = {}
    if "layout" in summary:  # NG (json2)
        for mem_type in summary["layout"]:
            if int(mem_type.get("used", 0)):
                used[mem_type["name"]] = int(mem_type["used"])
    else:  # legacy
        for key, val in summary.items():
            if key.startswith("used_") and isinstance(val, (int, float)) and val:
                used[key.removeprefix("used_").upper()] = int(val)
    return (int(total) if total is not None else None), used


# ── report + gate ────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--components-json", required=True, help="esp-idf-size --archives JSON")
    ap.add_argument("--summary-json", required=True, help="esp-idf-size summary JSON")
    ap.add_argument("--component-pattern", default=r"libtracer", help="archive-name regex")
    ap.add_argument("--target", default="unknown", help="chip target label (esp32c6, ...)")
    # Both ceilings are OFF by default: the footprint is tracked, not gated (maintainer
    # ruling). A ceiling on libtracer polices the wrong thing — what has to fit is the
    # PRODUCT image on its own target, and an esp32 must stay able to build the thinnest
    # client this library can serve. Every past raise was a feature landing, not a
    # regression. Pass a flag to arm a ceiling for a build that genuinely has one.
    ap.add_argument("--max-flash-bytes", type=int, default=None, help="opt-in ceiling")
    ap.add_argument("--max-static-ram-bytes", type=int, default=None, help="opt-in ceiling")
    ap.add_argument("--out-json", default=None, help="write the numbers as a JSON artifact")
    args = ap.parse_args()

    try:
        archives = _load_json(args.components_json)
        summary = _load_json(args.summary_json)
        arch_name, arch_info = find_component(archives, args.component_pattern)
        flash, ram, breakdown = component_sizes(arch_info)
        total, used = app_totals(summary)
    except Exception as exc:  # noqa: BLE001 — tooling breakage must be visible, not a crash
        print(f"::error::footprint reporter could not parse esp-idf-size output: {exc}")
        return 1

    flash_ok = args.max_flash_bytes is None or flash <= args.max_flash_bytes
    ram_ok = args.max_static_ram_bytes is None or ram <= args.max_static_ram_bytes
    gated = args.max_flash_bytes is not None or args.max_static_ram_bytes is not None
    verdict = ("PASS" if (flash_ok and ram_ok) else "FAIL") if gated else "REPORTED"

    lines = [
        f"## libtracer footprint — `full_node` @ {args.target}",
        "",
        f"Component archive: `{arch_name}` (from `esp-idf-size --archives`)",
        "",
        "| Scope | Flash (text+rodata) | Static RAM (data+bss+iram) |",
        "| --- | --- | --- |",
        f"| **libtracer component** | {_fmt(flash)} | {_fmt(ram)} |",
        f"| ceiling | "
        f"{'tracked, no ceiling' if args.max_flash_bytes is None else _fmt(args.max_flash_bytes)} | "
        f"{'tracked, no ceiling' if args.max_static_ram_bytes is None else _fmt(args.max_static_ram_bytes)} |",
        f"| whole app image (app + IDF) | {_fmt(total)} (total image) | "
        f"{_fmt(sum(v for k, v in used.items() if 'flash' not in k.lower()))} |",
        "",
        f"**Footprint: {verdict}**"
        + ("" if flash_ok else f" — component flash {_fmt(flash)} > {_fmt(args.max_flash_bytes)}")
        + ("" if ram_ok else f" — component static RAM {_fmt(ram)} > {_fmt(args.max_static_ram_bytes)}"),
        "",
        "<details><summary>libtracer per-memory-type breakdown</summary>",
        "",
        "| Memory type | Bytes |",
        "| --- | --- |",
        *(f"| {k} | {v:,} |" for k, v in sorted(breakdown.items(), key=lambda kv: -kv[1])),
        "",
        "</details>",
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
            "target": args.target,
            "component_archive": arch_name,
            "component_flash_bytes": flash,
            "component_static_ram_bytes": ram,
            "component_breakdown": breakdown,
            "app_total_image_bytes": total,
            "app_used_by_memory_type": used,
            # null ⇒ tracked, not gated — the shipped CI configuration for both.
            "thresholds": {
                "max_flash_bytes": args.max_flash_bytes,
                "max_static_ram_bytes": args.max_static_ram_bytes,
            },
            "verdict": verdict,
        }
        pathlib.Path(args.out_json).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    if flash_ok and ram_ok:
        return 0
    # Only reachable when the caller opted into a ceiling with an explicit flag.
    print(
        f"::error::libtracer footprint over the ceiling passed for {args.target}: "
        f"flash {_fmt(flash)} "
        f"({'tracked' if args.max_flash_bytes is None else 'max ' + _fmt(args.max_flash_bytes)}), "
        f"static RAM {_fmt(ram)} "
        f"({'tracked' if args.max_static_ram_bytes is None else 'max ' + _fmt(args.max_static_ram_bytes)})"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
