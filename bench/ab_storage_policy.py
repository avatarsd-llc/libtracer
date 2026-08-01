#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Interleaved A/B driver for `bench_storage_policy` — the RFC-0022 §3.C latency gate.

Two builds of the SAME bench source (one against the base, one against the branch) are
invoked **round-robin on one pinned core**: A, B, A, B, … Interleaving is the point — a
thermal ramp, a governor step or a noisy neighbour then lands in BOTH arms instead of being
attributed to the change. Each invocation prints one `RESULT` row per leg per round; this
medians them per (arm, leg).

The verdict is read against the **invariant control leg** (#464). `control-codec` touches no
graph settings and no vertex, so RFC-0022 cannot move it: if the control's own A/B ratio
drifts further from 1.0 than the effect claimed on any measured leg, the run measured the
machine and must be thrown away rather than reported.

Usage:
    python3 bench/ab_storage_policy.py --a <base-binary> --b <branch-binary> \\
        [--reps 16] [--rounds 3] [--core 3] [--raw samples.csv]
"""
from __future__ import annotations

import argparse
import re
import statistics
import subprocess
import sys

ROW = re.compile(r"^RESULT leg=(\S+) round=(\d+) ns_per_op=([0-9.]+)$", re.M)


def run(binary: str, core: int, rounds: int) -> list[tuple[str, float]]:
    cmd = ["taskset", "-c", str(core), binary, "--rounds", str(rounds)]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    return [(m.group(1), float(m.group(3))) for m in ROW.finditer(out)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="base (main) binary")
    ap.add_argument("--b", required=True, help="branch binary")
    ap.add_argument("--reps", type=int, default=16, help="invocations per arm (>= 15)")
    ap.add_argument("--rounds", type=int, default=3, help="rounds inside each invocation")
    ap.add_argument("--core", type=int, default=3)
    ap.add_argument("--raw", help="write every sample to this CSV")
    args = ap.parse_args()

    samples: dict[tuple[str, str], list[float]] = {}
    per_rep: dict[tuple[int, str, str], list[float]] = {}
    raw_rows = []
    for rep in range(args.reps):
        for arm, binary in (("A-main", args.a), ("B-branch", args.b)):
            for leg, ns in run(binary, args.core, args.rounds):
                samples.setdefault((arm, leg), []).append(ns)
                per_rep.setdefault((rep, arm, leg), []).append(ns)
                raw_rows.append(f"{rep},{arm},{leg},{ns}")
        print(f"  rep {rep + 1}/{args.reps} done", file=sys.stderr)

    if args.raw:
        with open(args.raw, "w", encoding="utf-8") as f:
            f.write("rep,arm,leg,ns_per_op\n")
            f.write("\n".join(raw_rows) + "\n")

    legs = sorted({leg for _, leg in samples})
    print(f"\ninterleaved A/B — {args.reps} invocations per arm x {args.rounds} rounds, "
          f"core {args.core}\n")
    print(f"{'leg':<16}{'A main (ns)':>14}{'B branch (ns)':>16}{'B/A':>9}{'n':>6}")
    ratios = {}
    for leg in legs:
        a = statistics.median(samples[("A-main", leg)])
        b = statistics.median(samples[("B-branch", leg)])
        ratios[leg] = b / a
        print(f"{leg:<16}{a:>14.3f}{b:>16.3f}{b / a:>9.3f}"
              f"{len(samples[('A-main', leg)]):>6}")

    # The floor is the CONTROL LEG'S OWN PER-REP SPREAD, not its median ratio (#464). The
    # median of a control ratio can land arbitrarily close to 1.0 by luck, which would make
    # any floor derived from it smaller than the run's real noise and turn a picosecond of
    # jitter into a reported "regression". What the control actually earns is: on this
    # machine, in this interleave, an untouched leg's paired A/B ratio wanders this far.
    def paired_ratios(leg: str) -> list[float]:
        out = []
        for rep in range(args.reps):
            a_ = statistics.median(per_rep[(rep, "A-main", leg)])
            b_ = statistics.median(per_rep[(rep, "B-branch", leg)])
            out.append(b_ / a_)
        return out

    ctl_excursions = sorted(abs(r - 1.0) for r in paired_ratios("control-codec"))
    # p90, not the max: one scheduling outlier must not licence an arbitrarily large floor
    # (a floor that loose would pass a real regression), and the median alone can land at
    # zero by luck. p90 is what an untouched leg wanders on 9 reps out of 10.
    floor = ctl_excursions[min(len(ctl_excursions) - 1, int(0.9 * len(ctl_excursions)))]
    measured = {leg: r for leg, r in ratios.items() if leg != "control-codec"}
    worst_slow = max((r - 1.0, leg) for leg, r in measured.items())
    print(f"\ncontrol floor   p90 |1 - B/A| over reps = {floor:.4f}  "
          f"(median {statistics.median(ctl_excursions):.4f}, max {ctl_excursions[-1]:.4f})")
    print(f"worst slowdown  B/A - 1                  = {worst_slow[0]:+.4f}  ({worst_slow[1]})")
    # DIRECTIONAL: only B slower than A is a regression. A leg that moved the other way is
    # reported, never celebrated — this instrument is not sized to claim a win.
    if worst_slow[0] <= floor:
        print("VERDICT PASS — no leg is slower by more than an UNTOUCHED leg wanders on this "
              "machine. A latency regression would have to clear that floor to be a finding.")
    else:
        print("VERDICT REGRESSION — a leg is slower than the control leg's own worst "
              "excursion. Read the per-leg rows; do not merge on this.")
    for leg, r in measured.items():
        if 1.0 - r > floor:
            print(f"        note: {leg} measured {(1 - r) * 100:.1f}% FASTER than main, past "
                  f"the control floor. Not claimed as a win — this instrument is sized to "
                  f"REFUSE a regression, not to bank an improvement.")
    d1 = statistics.median(samples[("B-branch", "settings-d1")])
    d8 = statistics.median(samples[("B-branch", "settings-d8")])
    print(f"depth check    settings-d8 / settings-d1 = {d8 / d1:.3f} "
          f"(one inline load is depth-independent; an ancestor walk is O(depth))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
