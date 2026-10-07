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
    # 2. bank: per-row spreads from those samples -> bench/aa_null.json (the gate reads it),
    #    with the replay of a SECOND measurement the fit did not use (held out) in its meta
    python3 bench/aa_null.py bank --raw raw-fit.json --held-out raw-eval.json \\
        --out bench/aa_null.json
    # 3. evaluate on any measurement: A/A false-fail rate and the detection rate of an
    #    injected 10% slowdown on each gated row
    python3 bench/aa_null.py evaluate --raw raw-eval.json --null bench/aa_null.json

The fit is MIN_ROUNDS (25) rounds or more, and `bank` refuses a shorter one. One round of
three builds takes about 145 s, so the fit is measured in several WINDOWS, one per stop of
the bench runner (each under about 45 minutes), and `--raw` is repeated once per window:
`pool` joins them so that no gate window of `PAIRS_DEFAULT` rounds straddles two stops, and
the null's meta records every window's date and round count. `aa_null_campaign.sh` is the
bench host's procedure for the whole bank (#1909).

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
MIN_ROUNDS = 25  # the shortest fit `bank` accepts (#1888: 15 rounds under-estimated spreads)
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


def _window_meta(raw: dict) -> list[dict]:
    """@brief The measuring windows one (pooled or single) measurement came from."""
    if "windows" in raw:
        return raw["windows"]
    return [{"date": raw.get("date", ""), "host": raw.get("host", ""),
             "rounds": raw["rounds"], "conditions": raw.get("conditions", "")}]


def pool(raws: list[dict]) -> dict:
    """@brief Join several measurements of the SAME builds, one per runner stop, into one.

    Each row's column per build is the windows' columns in order with one dropped round
    (None) between two windows, so a gate window of consecutive rounds never straddles two
    stops: `_windows` and @ref evaluate skip any window that holds a None. `rounds` is the
    measured total; the meta keeps each window's date and round count.
    @throws ValueError when the windows measured different builds.
    """
    if not raws:
        raise ValueError("no measurement to pool")
    builds = raws[0]["builds"]
    for r in raws[1:]:
        if r["builds"] != builds:
            raise ValueError(f"windows measured different builds: {builds} vs {r['builds']}")
    keys = sorted({k for r in raws for k in r["samples"]})
    samples: dict[str, list[list]] = {}
    for k in keys:
        cols: list[list] = [[] for _ in builds]
        for w, r in enumerate(raws):
            per_build = r["samples"].get(k) or [[None] * r["rounds"] for _ in builds]
            for b, col in enumerate(per_build):
                cols[b] += ([None] if w else []) + list(col)
        samples[k] = cols
    windows = [m for r in raws for m in _window_meta(r)]
    return {"builds": builds, "rounds": sum(r["rounds"] for r in raws), "samples": samples,
            "windows": windows, "date": windows[-1]["date"], "host": windows[0]["host"]}


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


def bank(raw: dict, width: int = pg.PAIRS_DEFAULT, held_out: dict | None = None) -> dict:
    """@brief The null file's content from one (or one pooled) measurement: {"meta": ...,
    "rows": {key: {leg: spread}}}. A leg with no complete session (or a zero column) is left
    out, and the gate then gates it on the flat threshold and says so.

    The meta records the rounds and the windows they came from, and, given @p held_out (a
    measurement the fit did not use), its replay: the A/A false-fail sessions and how many
    rows an injected INJECT slowdown fails in every session."""
    rows: dict[str, dict[str, float]] = {}
    for k, per_build in sorted(raw["samples"].items()):
        for leg in pg.LEGS:
            rs = session_ratios(per_build, leg, width)
            if rs:
                rows.setdefault(k, {})[leg] = round(robust_spread(rs), 5)
    meta = {"builds": raw["builds"], "rounds": raw["rounds"], "window": width,
            "k": pg.NULL_K, "k_cliff": pg.CLIFF_NULL_K, "floor": pg.NULL_FLOOR, "host": raw.get("host", ""),
            "banked": raw.get("date", ""), "windows": _window_meta(raw)}
    if held_out is not None:
        ev = evaluate(held_out, rows, width)
        meta["held_out"] = {
            "rounds": held_out["rounds"], "windows": _window_meta(held_out),
            "sessions": ev["sessions"], "false_fail_sessions": ev["false_fail_sessions"],
            "false_fails": ev["false_fails"], "rows": len(ev["detect"]),
            "rows_caught_every_session": sum(1 for c, t in ev["detect"].values() if c == t)}
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
            for s in range(0, min(len(per_build[x]), len(per_build[y])) - width + 1):
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


def _load(paths: list[str]) -> dict:
    """@brief One measuring window per path, pooled (@ref pool) when there are several."""
    raws = [json.loads(pathlib.Path(p).read_text()) for p in paths]
    return raws[0] if len(raws) == 1 else pool(raws)


def _cmd_bank(a: argparse.Namespace) -> int:
    raw = _load(a.raw)
    if raw["rounds"] < a.min_rounds:
        print(f"aa_null: {raw['rounds']} rounds is under the {a.min_rounds}-round fit; "
              f"measure more windows (repeat --raw) — {a.out} left as it was", file=sys.stderr)
        return 2
    out = bank(raw, a.window, _load(a.held_out) if a.held_out else None)
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(f"aa_null: {len(out['rows'])} rows banked from {raw['rounds']} rounds in "
          f"{len(out['meta']['windows'])} window(s) -> {a.out}")
    h = out["meta"].get("held_out")
    if h:
        print(f"held out ({h['rounds']} rounds): A/A FAIL in {h['false_fail_sessions']} of "
              f"{h['sessions']} sessions; injected x{INJECT:.2f} fails "
              f"{h['rows_caught_every_session']} of {h['rows']} rows in every session")
    for k, legs in out["rows"].items():
        if k.split("/")[0] in pg.CLIFF_MODES:
            continue
        print(f"  {k:<34} " + "  ".join(
            f"{leg} {s:.2%} -> x{pg.leg_factor(k, leg, out['rows'])[0]:.3f}"
            for leg, s in sorted(legs.items())))
    return 0


def _cmd_evaluate(a: argparse.Namespace) -> int:
    raw = _load(a.raw)
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
    b.add_argument("--raw", action="append", required=True,
                   help="a measurement (repeat: one per measuring window, pooled)")
    b.add_argument("--held-out", action="append", default=[],
                   help="a measurement the fit does not use, replayed into the meta (repeat)")
    b.add_argument("--min-rounds", type=int, default=MIN_ROUNDS)
    b.add_argument("--out", default=str(pg.NULL_FILE))
    b.add_argument("--window", type=int, default=pg.PAIRS_DEFAULT)
    b.set_defaults(fn=_cmd_bank)
    e = sub.add_parser("evaluate", help="replay the gate over a (held-out) measurement")
    e.add_argument("--raw", action="append", required=True)
    e.add_argument("--null", default=str(pg.NULL_FILE))
    e.add_argument("--window", type=int, default=pg.PAIRS_DEFAULT)
    e.set_defaults(fn=_cmd_evaluate)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
