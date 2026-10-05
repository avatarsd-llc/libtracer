#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Bank the perf gate's A/A null, and replay the gate over it (#1807).

The per-PR gate (`perf_gate.py`, paired mode) takes each row's threshold from a banked A/A
null: 3x the robust spread of the gate's own statistic between DIFFERENT builds of the SAME
source, floor 3%. Different builds, because the false failures this replaced (#1761, #1767,
#1772, #1855) were code-layout offsets between two builds, which run-to-run spread inside
one binary never shows. Building one source with different function alignments
(`-falign-functions=16/32/64`) moves every function the way an unrelated change does.

    # 1. measure: every family of every build, ABBA-rotated, R rounds; raw samples to JSON
    python3 bench/aa_null.py measure --build /tmp/a --build /tmp/f32 --build /tmp/f64 \\
        --rounds 10 --out raw-fit.json
    # 2. bank: per-row spreads from those samples -> bench/aa_null.json (the gate reads it)
    python3 bench/aa_null.py bank --raw raw-fit.json --out bench/aa_null.json
    # 3. evaluate on a SECOND measurement (held out): A/A false-fail rate and the detection
    #    rate of an injected 10% slowdown on each gated row
    python3 bench/aa_null.py evaluate --raw raw-eval.json --null bench/aa_null.json

`measure` runs exactly the steps the gate runs (`perf_gate.gate_plan`), pinned the same way
(`BENCH_CPU`, `BENCH_CPU_SINGLE`) and judged by the same condition check; a round that ran
contended is dropped for that step on every build. Re-bank on the bench host whenever its
CPU layout changes, and whenever a gated row is added.

The statistic, per row and leg: for each ordered pair of distinct builds (x, y) and each
window of `perf_gate.PAIRS_DEFAULT` consecutive clean rounds, the median of the per-round
ratios x/y — what one gate run comparing x to y would compute. Its robust spread is
1.4826 x the median |log ratio| (a sigma estimate centred on the A/A truth, 1.0), stored as
a relative figure. Stdlib only.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
import pathlib
import statistics
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import perf_gate as pg  # noqa: E402

MAD_SIGMA = 1.4826
INJECT = 1.10  # the synthetic regression evaluate() must catch: 10% slower on every leg


def measure(builds: list[pathlib.Path], rounds: int) -> dict:
    """@brief Time every gate step on every build, @p rounds times, rotating the start.

    @return {"builds": [...], "rounds": R, "samples": {key: [[metric|None per round]
            per build]}} — None marks a round dropped as contended.
    """
    bins = [pg._siblings(b / "bench_libtracer") for b in builds]
    plan = pg.gate_plan(*bins)
    samples: dict[str, list[list]] = {}
    for step in plan:
        rows_by: dict[str, list[list]] = {}
        for r in range(rounds):
            order = list(range(len(builds)))
            order = order[r % len(order):] + order[:r % len(order)]
            if r % 2:
                order.reverse()
            got, ok = {}, True
            for i in order:
                if i in step.args and step.key in bins[i]:
                    rows, clean = pg.run_step(bins[i][step.key], step, i)
                    got[i] = pg.step_metrics(step.key, rows)
                    ok = ok and clean
            for i, kv in got.items():
                for k, v in kv.items():
                    slot = rows_by.setdefault(k, [[None] * rounds for _ in builds])
                    slot[i][r] = v if ok else None
        samples.update(rows_by)
        print(f"  {step.label}: {rounds} rounds x {len(builds)} builds", flush=True)
    return {"builds": [str(b) for b in builds], "rounds": rounds, "samples": samples}


def _windows(xs: list, ys: list, width: int) -> list[list[tuple[float, float]]]:
    """@brief Every run of @p width consecutive rounds where both builds ran clean."""
    pairs = [(x, y) if x is not None and y is not None else None for x, y in zip(xs, ys)]
    out = []
    for s in range(0, len(pairs) - width + 1):
        w = pairs[s:s + width]
        if all(p is not None for p in w):
            out.append(w)
    return out


def session_ratios(per_build: list[list], leg: str, width: int) -> list[float]:
    """@brief The gate statistic of every A/A session one row affords: median per-round
    ratio x/y over each clean window, for every ordered pair of distinct builds."""
    vals = [[None if v is None else float(v[leg]) for v in b] for b in per_build]
    out = []
    for x, y in itertools.permutations(range(len(vals)), 2):
        for w in _windows(vals[x], vals[y], width):
            if all(a > 0 and b > 0 for a, b in w):
                out.append(statistics.median(a / b for a, b in w))
    return out


def robust_spread(ratios: list[float]) -> float:
    """@brief 1.4826 x the median |log ratio|, as a relative figure (0.02 = 2%)."""
    return math.expm1(MAD_SIGMA * statistics.median(abs(math.log(r)) for r in ratios))


def bank(raw: dict, width: int = pg.PAIRS_DEFAULT) -> dict:
    """@brief The null file's content from one measurement: {"meta": ..., "rows": {key:
    {leg: spread}}}. A leg with no complete session (or a zero column) is left out, and the
    gate then gates it on the flat threshold and says so."""
    rows: dict[str, dict[str, float]] = {}
    for k, per_build in sorted(raw["samples"].items()):
        for leg in pg.LEGS:
            rs = session_ratios(per_build, leg, width)
            if rs:
                rows.setdefault(k, {})[leg] = round(robust_spread(rs), 5)
    meta = {"builds": raw["builds"], "rounds": raw["rounds"], "window": width,
            "k": pg.NULL_K, "floor": pg.NULL_FLOOR, "host": raw.get("host", ""),
            "banked": raw.get("date", "")}
    return {"meta": meta, "rows": rows}


def _verdicts(cs: list[dict], bs: list[dict], k: str, null: dict, scale: float) -> list[str]:
    """@brief The failing legs of one A/A session, the candidate arm slowed by @p scale."""
    failed = []
    for leg in pg.LEGS:
        c = [float(x[leg]) for x in cs]
        b = [float(x[leg]) for x in bs]
        if min(c) <= 0 or min(b) <= 0:
            continue
        lower = leg == "deliv_s"
        c = [x / scale if lower else x * scale for x in c]
        if pg.leg_verdict(k, leg, c, b, null, pg.tick_guarded(k))[0]["fail"]:
            failed.append(leg)
    return failed


def evaluate(raw: dict, null: dict, width: int = pg.PAIRS_DEFAULT) -> dict:
    """@brief Replay the gate's verdict over every A/A session of @p raw.

    A session is one ordered build pair over one window of `width` rounds — one gate run.
    @return {"sessions": n, "false_fail_sessions": n, "false_fails": {key: n},
             "detect": {key: (caught, sessions)}} where `detect` injects INJECT on that
             key alone and counts the sessions that FAIL it.
    """
    gated = {f"{m}/{s}/{f}/{e}" for (_b, m, s, f, e) in pg.POINTS}
    samples = {k: v for k, v in raw["samples"].items()
               if k in gated or k.split("/")[0] in pg.CLIFF_MODES}
    nb = len(raw["builds"])
    sessions: dict[tuple, list[str]] = {}
    detect: dict[str, list[int]] = {}
    false_by_key: dict[str, int] = {}
    for k, per_build in samples.items():
        for x, y in itertools.permutations(range(nb), 2):
            for s in range(0, raw["rounds"] - width + 1):
                cs, bs = per_build[x][s:s + width], per_build[y][s:s + width]
                if any(v is None for v in cs + bs):
                    continue
                failed = _verdicts(cs, bs, k, null, 1.0)
                sessions.setdefault((x, y, s), [])
                if failed:
                    sessions[(x, y, s)].append(f"{k} {'+'.join(failed)}")
                    false_by_key[k] = false_by_key.get(k, 0) + 1
                d = detect.setdefault(k, [0, 0])
                d[1] += 1
                d[0] += bool(_verdicts(cs, bs, k, null, INJECT))
    return {"sessions": len(sessions),
            "false_fail_sessions": sum(1 for v in sessions.values() if v),
            "false_fails": false_by_key,
            "detect": {k: tuple(v) for k, v in detect.items()}}


def _cmd_measure(a: argparse.Namespace) -> int:
    raw = measure([pathlib.Path(b).resolve() for b in a.build], a.rounds)
    raw["date"] = time.strftime("%Y-%m-%d")
    raw["host"] = a.host
    raw["conditions"] = pg.LEDGER.line()
    pathlib.Path(a.out).write_text(json.dumps(raw) + "\n")
    print(f"aa_null: {len(raw['samples'])} keys -> {a.out} ({raw['conditions']})")
    return 0


def _cmd_bank(a: argparse.Namespace) -> int:
    out = bank(json.loads(pathlib.Path(a.raw).read_text()), a.window)
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(f"aa_null: {len(out['rows'])} rows banked -> {a.out}")
    for k, legs in out["rows"].items():
        if k.split("/")[0] in pg.CLIFF_MODES:
            continue
        print(f"  {k:<34} " + "  ".join(
            f"{leg} {s:.2%} -> x{pg.leg_factor(k, leg, out['rows'])[0]:.3f}"
            for leg, s in sorted(legs.items())))
    return 0


def _cmd_evaluate(a: argparse.Namespace) -> int:
    raw = json.loads(pathlib.Path(a.raw).read_text())
    null = json.loads(pathlib.Path(a.null).read_text()).get("rows", {})
    ev = evaluate(raw, null, a.window)
    n, bad = ev["sessions"], ev["false_fail_sessions"]
    print(f"A/A sessions (one gate run each): {n}; FAIL in {bad} "
          f"({bad / n:.1%} false-fail rate)" if n else "no complete A/A session")
    for k, c in sorted(ev["false_fails"].items()):
        print(f"  false FAIL: {k} in {c} session(s)")
    missed = {k: v for k, v in ev["detect"].items() if v[0] < v[1]}
    print(f"injected x{INJECT:.2f} on one row at a time: {len(ev['detect']) - len(missed)} of "
          f"{len(ev['detect'])} rows FAIL in every session")
    for k, (c, t) in sorted(missed.items()):
        thr = ", ".join(f"{leg} x{pg.leg_factor(k, leg, null)[0]:.3f}" for leg in pg.LEGS
                        if leg in null.get(k, {}))
        print(f"  missed: {k} caught {c}/{t} ({thr or 'flat'})")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("measure", help="time every gate step on every build")
    m.add_argument("--build", action="append", required=True,
                   help="a bench build directory (repeat; two or more layouts of one source)")
    m.add_argument("--rounds", type=int, default=10)
    m.add_argument("--host", default="", help="free text recorded with the samples")
    m.add_argument("--out", required=True)
    m.set_defaults(fn=_cmd_measure)
    b = sub.add_parser("bank", help="per-row spreads from a measurement -> the null file")
    b.add_argument("--raw", required=True)
    b.add_argument("--out", default=str(pg.NULL_FILE))
    b.add_argument("--window", type=int, default=pg.PAIRS_DEFAULT)
    b.set_defaults(fn=_cmd_bank)
    e = sub.add_parser("evaluate", help="replay the gate over a (held-out) measurement")
    e.add_argument("--raw", required=True)
    e.add_argument("--null", default=str(pg.NULL_FILE))
    e.add_argument("--window", type=int, default=pg.PAIRS_DEFAULT)
    e.set_defaults(fn=_cmd_evaluate)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
