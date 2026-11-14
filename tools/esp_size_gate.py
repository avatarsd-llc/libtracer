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

**Per-transport rows (#2401) are the exception, and they ARE pinned.** With
`--files-json` (the `esp-idf-size --files` output of the same map), each transport's
object files are summed as they sit in the linked image, after `--gc-sections`, and
compared on esp32c6 against the pins in TRANSPORT_ROWS. Growth past a pin is
`::error::` and exit 1, so a transport cannot get fatter unseen; the component total
above stays tracked. A pin holds only for the compiler it was measured with
(PINNED_TOOLCHAIN, passed in as `--toolchain`). On esp32c6 another compiler fails every
pinned row: esp-idf.yml builds these jobs in an image pinned by digest, so a mismatch means
the image moved or a pin went stale, and a compiler upgrade arrives as a deliberate image
bump that re-pins every row (#2830). Another target reports the rows without comparing
them (#1138). A transport the `full_node` image compiles but never links, or never
compiles, says so instead of reading 0 B. Those (CAN, WebSocket) are pinned on a second,
footprint-only image instead (`--image transport_footprint`, #2678), which links them;
there the rows full_node pins are reported only. A pinned row that reads 0 B fails: it is
no longer linked.

Exit 0 = report published (and within any ceiling that was passed), 1 = broken
input, over a ceiling the caller opted into, a transport row over its pin, or an esp32c6
build by a compiler other than PINNED_TOOLCHAIN. Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sys
from typing import Any


# Per-transport rows (#2401): (row, object stems in libtracer.a, esp32c6 pin in flash bytes,
# or the reason the full_node image carries no such row). Pins are the linked flash of those
# objects in full_node @ esp32c6, measured in espressif/idf:release-v6.0 (esp-idf.yml's
# full-node job) with PINNED_TOOLCHAIN. A row's bytes are what its own objects keep after
# --gc-sections; code they share with the rest of the library (rope, frame, the graph) is
# counted once, in the object that defines it.
PINNED_TARGET = "esp32c6"
PINNED_TOOLCHAIN = "riscv32-esp-elf-g++ (crosstool-NG esp-15.2.0_20251204) 15.2.0"
TRANSPORT_ROWS = (
    # 2,710 -> 2,608 B (#2147): the receive thread body becomes one readiness callback.
    ("datagram (UDP)", ("transport_udp", "builtin_transport_udp"), 2608, None),
    # 9,216 -> 9,446 B (#2147): a frame read resumes across readiness turns, so frame ingress is
    # out of line and the connection and listener each gain a callback (maintainer ruling 60).
    ("stream (TCP)", ("transport_tcp", "builtin_transport_tcp"), 9446, None),
    # 8,896 -> 9,080 B (#2147): the poll loops give way to turn functions, the write-interest
    # arm and disarm, and the thread-per-link fallback's block (maintainer ruling 60).
    # 9,080 -> 9,262 B: ruling 73 (2026-11-11), #2751 teardown fix; follow-up #2801 buys back
    # ~110 B. Ruled at 9,264; pinned at the 9,262 measured on the rebased batch.
    # 9,262 -> 9,184 B (#2801): one shut-down walk of the sessions, the flat "any session
    # open" read folded into the teardown's own lock, one shut_down override for both servers.
    # 9,184 -> 9,296 B: ruling 76 (2026-11-12), #2812: the one-peer liveness check owes RX
    # too (a dialer closes a server frozen mid-frame); two atomic flag calls in sweep_one.
    ("socket endpoint (shared by UDP and TCP)", ("posix_endpoint",), 9296, None),
    ("CAN (transport_can + TWAI link)", ("transport_can", "twai_link", "socketcan_link_stub"),
     None, "compiled, not linked: full_node creates no CAN connection, so --gc-sections drops it"),
    ("WebSocket (httpd server + client link)", ("httpd_ws_link", "esp_ws_client_link"),
     None, "compiled, not linked: full_node creates no WebSocket connection"),
    ("Noise link", (), None,
     "not built: header-only, and full_node leaves LIBTRACER_NOISE_CRYPTO_PSA off"),
)

# Rows pinned on the footprint-only image (#2678): integrations/esp-idf/examples/
# transport_footprint links the two transports full_node drops, so their rows are pinned
# there, measured the same way (esp32c6, espressif/idf:release-v6.0, PINNED_TOOLCHAIN). The
# rows full_node pins stay pinned on full_node only; here they are reported.
FOOTPRINT_IMAGE = "transport_footprint"
FOOTPRINT_PINS = {
    # 14,248 B at this PR's first base; +20 B from #2704 (CAN egress sends through the native
    # gather path), which landed first. Measured on the serial run at 119d76e6.
    # 14,268 -> 14,290 B: ruling 73 (2026-11-11), #2751 teardown fix; follow-up #2801 buys
    # back ~110 B.
    "CAN (transport_can + TWAI link)": 14290,
    # 33,387 -> 33,513 B: ruling 73 (2026-11-11), #2751 teardown fix; follow-up #2801 buys
    # back ~110 B.
    "WebSocket (httpd server + client link)": 33513,
}


def row_pin(image: str, row: str, pin: int | None, why: str | None) -> tuple[int | None, str | None]:
    """Return the (pin, reason for no pin) of @p row in @p image's build."""
    if image != FOOTPRINT_IMAGE:
        return pin, why
    if row in FOOTPRINT_PINS:
        return FOOTPRINT_PINS[row], None
    return None, ("pinned on full_node, not on this image" if pin is not None
                  else "pinned on neither image")


def transport_rows(files: dict[str, Any]) -> dict[str, int]:
    """Return {row: linked flash bytes} summed over each row's objects in `--files` output.

    Keys look like `esp-idf/libtracer/libtracer.a:transport_udp.cpp.obj`; an object absent
    from the image (dropped by --gc-sections, or never compiled) adds nothing.
    """
    flash_by_stem: dict[str, int] = {}
    for name, info in files.items():
        m = re.search(r"libtracer\.a:([A-Za-z0-9_]+)\.cpp\.obj$", name)
        if m:
            flash_by_stem[m.group(1)] = component_sizes(info)[0]
    if not flash_by_stem:
        raise KeyError("no libtracer.a object in the --files output")
    return {row: sum(flash_by_stem.get(st, 0) for st in stems)
            for row, stems, _pin, _why in TRANSPORT_ROWS}


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
    ap.add_argument("--files-json", default=None,
                    help="esp-idf-size --files JSON of the same map: enables the transport rows")
    ap.add_argument("--image", default="full_node", choices=("full_node", FOOTPRINT_IMAGE),
                    help=f"the example the map came from: full_node or {FOOTPRINT_IMAGE} (#2678)")
    ap.add_argument("--toolchain", default="unknown",
                    help=f"the compiler's --version first line; on {PINNED_TARGET} it must be "
                         "PINNED_TOOLCHAIN, or every pinned row fails")
    args = ap.parse_args()

    try:
        archives = _load_json(args.components_json)
        summary = _load_json(args.summary_json)
        arch_name, arch_info = find_component(archives, args.component_pattern)
        flash, ram, breakdown = component_sizes(arch_info)
        total, used = app_totals(summary)
        rows = transport_rows(_load_json(args.files_json)) if args.files_json else None
    except Exception as exc:  # noqa: BLE001 — tooling breakage must be visible, not a crash
        print(f"::error::footprint reporter could not parse esp-idf-size output: {exc}")
        return 1

    flash_ok = args.max_flash_bytes is None or flash <= args.max_flash_bytes
    ram_ok = args.max_static_ram_bytes is None or ram <= args.max_static_ram_bytes
    gated = args.max_flash_bytes is not None or args.max_static_ram_bytes is not None
    verdict = ("PASS" if (flash_ok and ram_ok) else "FAIL") if gated else "REPORTED"

    lines = [
        f"## libtracer footprint — `{args.image}` @ {args.target}",
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
    # Pins compare on esp32c6 only; another target reports its rows (#1138). On esp32c6 a pin
    # holds for PINNED_TOOLCHAIN only, and the size-gate jobs build in an image pinned by digest
    # that carries it: another compiler there fails each pinned row instead of reporting it,
    # so a moved image cannot leave the pins unread (#2830).
    rows_compared = args.target == PINNED_TARGET
    toolchain_ok = args.toolchain == PINNED_TOOLCHAIN
    rows_over: list[str] = []
    rows_json: list[dict[str, Any]] = []
    if rows is not None:
        lines += [
            "### Per-transport flash rows (linked objects in this image)",
            "",
            f"| Transport | Flash | {PINNED_TARGET} pin | Verdict |",
            "| --- | --- | --- | --- |",
        ]
        for row, _stems, full_node_pin, full_node_why in TRANSPORT_ROWS:
            got = rows[row]
            pin, why = row_pin(args.image, row, full_node_pin, full_node_why)
            if why is not None and args.image == FOOTPRINT_IMAGE:
                # Pinned on full_node: this image only reports it.
                state = f"{_fmt(got)}; {why}" if got else f"not linked; {why}"
                lines.append(f"| {row} | {state} | n/a | not measured here |")
                rows_json.append({"row": row, "linked": got != 0, "flash_bytes": got,
                                  "reason": why})
                continue
            if why is not None:
                # A row with no pin must stay out of the image; if it shows up, it needs one.
                state = why if got == 0 else f"now linked ({_fmt(got)}): pin it"
                lines.append(f"| {row} | {state} | n/a | not measured here |")
                rows_json.append({"row": row, "linked": got != 0, "flash_bytes": got,
                                  "reason": why})
                if got:
                    print(f"::warning::transport row {row!r} is linked now; give it a pin (#2401)")
                continue
            if not rows_compared:
                verdict_row = f"REPORTED (pins hold for {PINNED_TARGET} only)"
            elif not toolchain_ok:
                verdict_row = "FAIL (not the pinned toolchain)"
                rows_over.append(f"{row}: not the pinned toolchain")
            elif got == 0:
                # A pin on a row the image does not link would pass forever on 0 B.
                verdict_row = "FAIL (not linked)"
                rows_over.append(f"{row}: not linked, pin {pin:,} B")
            elif got > pin:
                verdict_row = "FAIL"
                rows_over.append(f"{row}: {got:,} B > pin {pin:,} B")
            else:
                verdict_row = "PASS" if got == pin else f"PASS ({pin - got:,} B under: re-pin down)"
            lines.append(f"| {row} | {_fmt(got)} | {_fmt(pin)} | {verdict_row} |")
            rows_json.append({"row": row, "linked": True, "flash_bytes": got, "pin_bytes": pin,
                              "verdict": verdict_row})
        lines += [f"Toolchain: `{args.toolchain}`; pins measured with `{PINNED_TOOLCHAIN}`.", ""]
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
            "toolchain": args.toolchain,
            "transport_rows": rows_json,
        }
        pathlib.Path(args.out_json).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    if rows_over:
        print(f"::error::libtracer per-transport flash rows failed their {PINNED_TARGET} pins "
              f"(#2401; built with {args.toolchain!r}, pinned with {PINNED_TOOLCHAIN!r}): "
              + "; ".join(rows_over))
        return 1
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
