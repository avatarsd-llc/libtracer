#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The counted-metrics ledger (#2137): the gate the peer-path work (#2136) is judged by.

Five metric families, one JSON pin file each under `bench/ledger/`:

  copies        user-space copies per relayed frame, at 64 B / 1 KiB / 4 KiB / 16 KiB
  syscalls      socket and I/O syscalls per relayed frame
  switches      task switches per relayed frame
  ram_per_peer  RAM per peer (and the UDP link at rest)
  idle_wakeups  wakeups per second with no traffic

Every row is pinned for one target. WIDE rows (the x86 host) are MEASURED by
`bench_peer_counts` (or, for RAM, by `bench_conn_ram`); MID rows (the ESP32-C6) are
STATIC counts read from the code, each with the reading that produced it in `basis`.
Every row also carries the goal #2136 sets for it (`goal`, null where the spec gives no
number and the row only has to hold) and the change that last moved it (`moved_by`).

The WIDE RAM rows are not pinned in the ledger: they name a row of
`bench/ram_census_pins.json` and read its value, band and enforcement mode from there, so
one number has one pin.

Subcommands, and what each guarantees:

  `gate`  A measured row above its pin by more than the family band FAILS (a raise). A row
          below its pin by more than the band also FAILS, with the re-pin instruction: a pin
          left above the truth makes the next growth free (the symbol ratchet's rule). A
          pinned row the transcript does not contain fails in either mode: a failure to
          measure is not a verdict. A run on another toolchain than the pin's is reported in
          warn mode, as `ram_census.py` does.
  `diff`  Compares the pins against a base revision. A pin whose value RISES fails unless
          the row carries a new `waiver`; a value that changes without a new `moved_by`
          fails, so the ledger always names the change that moved a row; a row may not
          disappear. This is what makes a static MID count a ratchet: its pin is the count.
  `pins`  Re-pins the measured WIDE rows from a transcript (`--write` rewrites the files).
          It never edits `moved_by`; the author names the change, and `diff` checks it.
  `emit`  Merges the measured rows into the bench-local chart-store JSON on a push to
          `main`, so each merge appends one point per row to the history.
  `render` Prints the perf page's ledger section from the pins and that history.

    ./counted_ledger.py gate --raw peer.txt --conn-raw conn.txt
    ./counted_ledger.py diff --base origin/main
    ./counted_ledger.py pins --raw peer.txt --write
    ./counted_ledger.py emit --raw peer.txt --merge bench/benchmark_output.json
    ./counted_ledger.py render --history dev/bench-local/data.js

Stdlib only. Transcript rows: `RESULT arm=<arm> metric=<metric> n=N median=V min=V max=V`.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

import host_guard  # noqa: E402  (compiler identity, shared with the other ratchets)
import ram_census  # noqa: E402  (the RAM pins' own band rule)

LEDGER = HERE / "ledger"
RAM_PINS = HERE / "ram_census_pins.json"
# Page order. A family file not named here is an error, not a silent omission.
FAMILIES = ("copies", "syscalls", "switches", "ram_per_peer", "idle_wakeups")
SERIES_PREFIX = "ledger"

ROW = re.compile(
    r"^RESULT\s+arm=(\S+)\s+metric=(\S+)\s+n=(\d+)\s+"
    r"median=(-?[\d.]+)\s+min=(-?[\d.]+)\s+max=(-?[\d.]+)\s*$",
    re.MULTILINE,
)


# --- loading ---------------------------------------------------------------------------

def parse_rows(text: str) -> dict[tuple[str, str], dict[str, float]]:
    """@brief `RESULT arm=… metric=…` rows -> {(arm, metric): {n, median, min, max}}."""
    out: dict[tuple[str, str], dict[str, float]] = {}
    for m in ROW.finditer(text):
        arm, metric, n, med, lo, hi = m.groups()
        out[(arm, metric)] = {"n": int(n), "median": float(med), "min": float(lo), "max": float(hi)}
    return out


def read_transcript(path: str | None) -> tuple[dict, str]:
    """@brief Parse a transcript path; returns ({}, reason) when it yields nothing."""
    if not path:
        return {}, "not provided"
    p = pathlib.Path(path)
    if not p.exists():
        return {}, f"{p} does not exist"
    rows = parse_rows(p.read_text())
    return (rows, "") if rows else ({}, f"{p} contains no RESULT rows")


def check_family(fam: dict, where: str) -> None:
    """@brief Refuse a pin file whose rows the gate could misread."""
    seen: set[str] = set()
    for r in fam["rows"]:
        for key in ("id", "target", "source", "goal", "moved_by"):
            if key not in r:
                raise SystemExit(f"{where}: row {r.get('id')!r} has no {key!r}")
        if r["id"] in seen:
            raise SystemExit(f"{where}: duplicate row id {r['id']!r}")
        seen.add(r["id"])
        if r["target"] not in ("WIDE", "MID", "NARROW"):
            raise SystemExit(f"{where}: row {r['id']!r} has unknown target {r['target']!r}")
        if r["source"] == "measured" and not ("arm" in r and "metric" in r and "value" in r):
            raise SystemExit(f"{where}: measured row {r['id']!r} needs arm, metric and value")
        if r["source"] == "static" and not ("value" in r and r.get("basis")):
            raise SystemExit(f"{where}: static row {r['id']!r} needs a value and the basis it was read from")
        if r["source"] == "ram_census" and ("value" in r or "arm" not in r or "metric" not in r):
            raise SystemExit(f"{where}: ram_census row {r['id']!r} names an arm and metric and "
                             f"carries no value of its own (one number, one pin)")
        if r["source"] not in ("measured", "static", "ram_census"):
            raise SystemExit(f"{where}: row {r['id']!r} has unknown source {r['source']!r}")


def load_texts(texts: dict[str, str], ram_text: str | None) -> list[dict]:
    """@brief Families from raw JSON texts (keyed by family), RAM rows resolved."""
    if extra := sorted(set(texts) - set(FAMILIES)):
        raise SystemExit(f"counted_ledger: unknown family file(s): {', '.join(extra)}")
    ram = json.loads(ram_text) if ram_text else {"pins": []}
    ram_by = {(p["arm"], p["metric"]): p["value"] for p in ram["pins"]}
    out = []
    for name in FAMILIES:
        if name not in texts:
            continue
        fam = json.loads(texts[name])
        check_family(fam, f"ledger/{name}.json")
        for r in fam["rows"]:
            if r["source"] == "ram_census":
                r["value"] = ram_by.get((r["arm"], r["metric"]))
        out.append(fam)
    return out


def load_ledger(ledger: pathlib.Path = LEDGER, ram_pins: pathlib.Path = RAM_PINS) -> list[dict]:
    """@brief The ledger as it is on disk."""
    texts = {p.stem: p.read_text() for p in sorted(ledger.glob("*.json"))}
    ram = ram_pins.read_text() if ram_pins.exists() else None
    return load_texts(texts, ram)


def load_ledger_at(rev: str, ledger: pathlib.Path = LEDGER, ram_pins: pathlib.Path = RAM_PINS) -> list[dict]:
    """@brief The ledger as it was at git revision @p rev (families absent there are skipped)."""
    def show(path: pathlib.Path) -> str | None:
        rel = path.resolve().relative_to(REPO).as_posix()
        p = subprocess.run(["git", "show", f"{rev}:{rel}"], capture_output=True, text=True, cwd=REPO)
        return p.stdout if p.returncode == 0 else None
    texts = {n: t for n in FAMILIES if (t := show(ledger / f"{n}.json")) is not None}
    return load_texts(texts, show(ram_pins))


# --- the verdict rules -----------------------------------------------------------------

def band_for(value: float, fam: dict) -> float:
    """@brief The allowed absolute drift for one row: the larger of the two family terms."""
    return max(float(fam.get("band_abs", 0)), abs(float(value)) * float(fam.get("band_pct", 0)) / 100.0)


def verdict(measured: float, pinned: float, band: float) -> str:
    """@brief `ok`, `GREW` (a raise: fails) or `SHRANK` (an improvement: re-pin)."""
    delta = measured - pinned
    # A hair of slack for the two-decimal transcript, so a band of 0 means "exact".
    if abs(delta) <= band + 1e-9:
        return "ok"
    return "GREW" if delta > 0 else "SHRANK"


def gate_rows(ledger: list[dict], peer: dict, conn: dict, ram: dict, toolchain: str,
              mode: str) -> tuple[list[str], list[str], list[str]]:
    """@brief Judge every measured row. Returns (table lines, hard failures, ratchet drifts).

    Hard failures (a row not measured) fail in either mode. Drifts fail in the mode the
    row's family may enforce: `mode`, downgraded to warn across toolchains, and for a RAM
    row never stronger than `ram_census_pins.json`'s own activation mode.
    """
    table: list[str] = []
    hard: list[str] = []
    drift: list[tuple[str, str]] = []
    ram_mode = ram.get("activation", {}).get("mode", "warn")
    for fam in ledger:
        fam_mode = mode
        if fam.get("toolchain") and fam["toolchain"] != toolchain:
            fam_mode = "warn"
            print(f"::notice::counted_ledger: {fam['family']} pinned on {fam['toolchain']!r}, "
                  f"measured on {toolchain!r}: warn mode only")
        for r in fam["rows"]:
            if r["source"] == "static":
                continue
            src, row_mode = (peer, fam_mode) if r["source"] == "measured" else (conn, "warn")
            if r["source"] == "ram_census":
                if ram_mode == "fail" and fam_mode == "fail":
                    row_mode = "fail"
                if r["value"] is None:
                    hard.append(f"{fam['family']}/{r['id']}: {r['arm']}/{r['metric']} has no pin "
                                f"in ram_census_pins.json")
                    continue
            got = src.get((r["arm"], r["metric"]))
            if got is None:
                if r["source"] == "ram_census" and not conn:
                    table.append(f"  {fam['family']}/{r['id']}\tnot measured in this run (no --conn-raw)")
                    continue
                hard.append(f"{fam['family']}/{r['id']}: {r['arm']}/{r['metric']} is pinned but "
                            f"absent from the transcript")
                continue
            if r["source"] == "ram_census":
                band = float(ram_census.band_for({"value": r["value"]}, ram))
            else:
                band = band_for(r["value"], fam)
            mark = verdict(got["median"], r["value"], band)
            table.append(f"  {fam['family']}/{r['id']}\t{got['median']:g}\tpinned {r['value']:g}\t"
                         f"{got['median'] - r['value']:+g}\t(band ±{band:g}, spread "
                         f"{got['max'] - got['min']:g})\t{mark}")
            if mark == "GREW":
                drift.append((row_mode, f"{fam['family']}/{r['id']} rose {r['value']:g} -> "
                                        f"{got['median']:g}, past the ±{band:g} band"))
            elif mark == "SHRANK":
                drift.append((row_mode, f"{fam['family']}/{r['id']} fell {r['value']:g} -> "
                                        f"{got['median']:g}: an improvement, RE-PIN it and name "
                                        f"the change in moved_by"))
    fails = [d for m, d in drift if m == "fail"]
    warns = [d for m, d in drift if m != "fail"]
    return table, hard + fails, warns


def diff_rows(base: list[dict], head: list[dict]) -> list[str]:
    """@brief The pin-file ratchet: what a PR may not do to the pins."""
    problems: list[str] = []
    base_by = {(f["family"], r["id"]): r for f in base for r in f["rows"]}
    head_by = {(f["family"], r["id"]): r for f in head for r in f["rows"]}
    for key, b in sorted(base_by.items()):
        h = head_by.get(key)
        name = f"{key[0]}/{key[1]}"
        if h is None:
            problems.append(f"{name}: the row was removed. A ledger row may not disappear; a row "
                            f"whose metric no longer exists is pinned at its final value instead")
            continue
        bv, hv = b.get("value"), h.get("value")
        if bv is None or hv is None or bv == hv:
            continue
        if hv > bv and (not h.get("waiver") or h.get("waiver") == b.get("waiver")):
            problems.append(f"{name}: the pin rose {bv:g} -> {hv:g}. A raise needs a new `waiver` "
                            f"on the row naming the ruling that accepts it")
        if h["source"] != "ram_census" and h["moved_by"] == b["moved_by"]:
            problems.append(f"{name}: the pin moved {bv:g} -> {hv:g} but `moved_by` still reads "
                            f"{b['moved_by']!r}. Name the change that moved it")
    return problems


# --- subcommands -----------------------------------------------------------------------

def _cmd_gate(args: argparse.Namespace) -> int:
    ledger = load_ledger(pathlib.Path(args.ledger), pathlib.Path(args.ram_pins))
    ram = json.loads(pathlib.Path(args.ram_pins).read_text())
    peer, why = read_transcript(args.raw)
    conn, _ = read_transcript(args.conn_raw)
    toolchain = host_guard.compiler_identity(args.cxx)
    print(f"COUNTED_LEDGER\tmode={args.mode}\ttoolchain={toolchain}")
    if not peer:
        print(f"COUNTED_LEDGER\tFAIL\treason=no-measurement ({why})")
        print("  A failure to measure is not a verdict. Warn mode does not answer for a run")
        print("  that did not happen.")
        return 1
    table, fails, warns = gate_rows(ledger, peer, conn, ram, toolchain, args.mode)
    print("\n".join(table))
    for w in warns:
        print(f"::warning::counted ledger drift (warn mode): {w}")
    if not fails:
        print("COUNTED_LEDGER\tPASS" + (f" ({len(warns)} warning(s))" if warns else ""))
        return 0
    for f in fails:
        print(f"::error::counted ledger: {f}")
    print("COUNTED_LEDGER\tFAIL")
    print("  Re-pin only by a deliberate PR edit: `counted_ledger.py pins --raw <transcript>")
    print("  --write` rewrites the measured values; name the change in each moved row's")
    print("  `moved_by`. A raise also needs a `waiver` on the row (see `diff`).")
    return 1


def _cmd_diff(args: argparse.Namespace) -> int:
    head = load_ledger(pathlib.Path(args.ledger), pathlib.Path(args.ram_pins))
    base = load_ledger_at(args.base, pathlib.Path(args.ledger), pathlib.Path(args.ram_pins))
    problems = diff_rows(base, head)
    rows = sum(len(f["rows"]) for f in head)
    if not problems:
        print(f"COUNTED_LEDGER_DIFF\tPASS\t{rows} rows against {args.base}")
        return 0
    for p in problems:
        print(f"::error::counted ledger pins: {p}")
    print(f"COUNTED_LEDGER_DIFF\tFAIL\t{len(problems)} problem(s) against {args.base}")
    return 1


def _cmd_pins(args: argparse.Namespace) -> int:
    peer, why = read_transcript(args.raw)
    if not peer:
        print(f"counted_ledger: cannot re-pin: {why}", file=sys.stderr)
        return 2
    ledger_dir = pathlib.Path(args.ledger)
    for name in FAMILIES:
        path = ledger_dir / f"{name}.json"
        if not path.exists():
            continue
        fam = json.loads(path.read_text())
        moved = []
        for r in fam["rows"]:
            got = peer.get((r.get("arm"), r.get("metric"))) if r["source"] == "measured" else None
            if got is None:
                continue
            v = got["median"]
            v = int(v) if float(v).is_integer() else v
            if v != r["value"]:
                moved.append(f"  {name}/{r['id']}: {r['value']} -> {v}")
                r["value"] = v
        print("\n".join(moved) if moved else f"  {name}: no measured row moved")
        if args.write and moved:
            path.write_text(json.dumps(fam, indent=2) + "\n")
    if args.write:
        print("Now name the change in `moved_by` on every moved row; `diff` checks it.")
    return 0


def series_name(fam: str, arm: str, metric: str) -> str:
    """@brief The persisted chart-store name of one ledger row. Renaming one orphans its history."""
    return f"{SERIES_PREFIX} {fam} {arm} {metric}"


def _cmd_emit(args: argparse.Namespace) -> int:
    peer, why = read_transcript(args.raw)
    if not peer:
        print(f"::notice::counted_ledger: no transcript to record ({why})")
        return 0
    ledger = load_ledger(pathlib.Path(args.ledger), pathlib.Path(args.ram_pins))
    series = []
    for fam in ledger:
        for r in fam["rows"]:
            got = peer.get((r.get("arm"), r.get("metric"))) if r["source"] == "measured" else None
            # A zero has no ratio for the chart store to trend against; the gate asserts it.
            if got is not None and got["median"] != 0:
                series.append({"name": series_name(fam["family"], r["arm"], r["metric"]),
                               "unit": fam["unit"], "value": got["median"]})
    merge = pathlib.Path(args.merge)
    existing = json.loads(merge.read_text()) if merge.exists() else []
    have = {m["name"] for m in existing}
    fresh = [m for m in series if m["name"] not in have]
    merge.write_text(json.dumps(existing + fresh, indent=2) + "\n")
    print(f"counted_ledger: merged {len(fresh)} ledger metrics into {merge}")
    return 0


# --- the perf page section -------------------------------------------------------------

def banked(history: dict | None) -> dict[str, list[tuple[str, float]]]:
    """@brief {series name: [(commit sha, value), ...] oldest first} for every ledger series."""
    out: dict[str, list[tuple[str, float]]] = {}
    for runs in (history or {}).get("entries", {}).values():
        for run in runs:
            sha = run.get("commit", {}).get("id", "")
            for b in run.get("benches", []):
                if b.get("name", "").startswith(SERIES_PREFIX + " "):
                    out.setdefault(b["name"], []).append((sha, float(b["value"])))
    return out


def _fmt(v: float | None, fam: dict, r: dict) -> str:
    """@brief One value as the page prints it: copies as a multiple of the payload."""
    if v is None:
        return "—"
    if fam.get("per_payload") and r.get("payload"):
        return f"{v / r['payload']:.2f}× ({v:g} B)"
    return f"{v:g}"


def render_markdown(ledger: list[dict], history: dict | None) -> str:
    """@brief The ledger section: one table per family, generated from pins and history."""
    hist = banked(history)
    parts = []
    for fam in ledger:
        lines = [f"### {fam['title']}", "",
                 f"Unit: {fam['unit']}{' (shown as a multiple of the payload)' if fam.get('per_payload') else ''}. "
                 f"Pins: `bench/ledger/{fam['family']}.json`."
                 + (f" Goals: {fam['_goal']}" if fam.get("_goal") else ""), "",
                 "| Row | Target | Source | Current | Goal (#2136) | Moved by | Banked on `main` |",
                 "| --- | --- | --- | --- | --- | --- | --- |"]
        for r in fam["rows"]:
            pts = hist.get(series_name(fam["family"], r.get("arm", ""), r.get("metric", "")), [])
            trend = (f"{_fmt(pts[0][1], fam, r)} at `{pts[0][0][:8]}` → {_fmt(pts[-1][1], fam, r)}, "
                     f"{len(pts)} point(s)") if pts else "—"
            source = {"measured": "measured", "static": "static count",
                      "ram_census": "measured (`ram_census_pins.json`)"}[r["source"]]
            goal = "hold" if r["goal"] is None else _fmt(r["goal"], fam, r)
            lines.append(f"| {r['id']} | {r['target']} | {source} | {_fmt(r['value'], fam, r)} | "
                         f"{goal} | {r['moved_by']} | {trend} |")
        parts.append("\n".join(lines))
    return "\n\n".join(parts)


def _cmd_render(args: argparse.Namespace) -> int:
    history = None
    if args.history and pathlib.Path(args.history).exists():
        raw = pathlib.Path(args.history).read_text().strip()
        history = json.loads(raw.removeprefix("window.BENCHMARK_DATA = ").rstrip(";"))
    print(render_markdown(load_ledger(pathlib.Path(args.ledger), pathlib.Path(args.ram_pins)), history))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ledger", default=str(LEDGER), help="the pin-file directory")
    ap.add_argument("--ram-pins", default=str(RAM_PINS), help="the RAM pins the WIDE RAM rows read")
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gate", help="judge a transcript against the measured pins")
    g.add_argument("--raw", help="bench_peer_counts transcript")
    g.add_argument("--conn-raw", help="bench_conn_ram transcript (for the WIDE RAM rows)")
    g.add_argument("--mode", choices=("warn", "fail"), default="fail")
    g.add_argument("--cxx", default=None, help="the compiler whose identity attributes the pins")
    g.set_defaults(func=_cmd_gate)

    d = sub.add_parser("diff", help="the pin-file ratchet against a base revision")
    d.add_argument("--base", required=True, help="git revision to compare the pins against")
    d.set_defaults(func=_cmd_diff)

    p = sub.add_parser("pins", help="re-pin the measured rows from a transcript")
    p.add_argument("--raw", required=True)
    p.add_argument("--write", action="store_true", help="rewrite the pin files")
    p.set_defaults(func=_cmd_pins)

    e = sub.add_parser("emit", help="merge the measured rows into the chart-store JSON")
    e.add_argument("--raw")
    e.add_argument("--merge", default=str(HERE / "benchmark_output.json"))
    e.set_defaults(func=_cmd_emit)

    r = sub.add_parser("render", help="print the perf page's ledger section")
    r.add_argument("--history", help="a benchmark-action data.js (the bench-local store)")
    r.set_defaults(func=_cmd_render)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
