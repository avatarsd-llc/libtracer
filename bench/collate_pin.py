#!/usr/bin/env python3
"""RFC-0022 §6 collator: per-arm min/median/max ACROSS ROUNDS, never a single round's figure.

SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

Reads `RESULT_PIN` / `RESULT_PINNET` lines from stdin or files and renders the two tables the
gate needs.  The comparison rule is the standing one and it is enforced HERE rather than left
to the reader: an arm beats the control only when the per-round paired delta `arm - control`
carries the same sign in EVERY round.  One flip prints `indistinguishable`, and no amount of
median separation upgrades that.  See `verdict` for why the test is paired and not an
unpaired range overlap.
"""
import sys
import statistics
from collections import defaultdict


def rng(xs):
    """min / median / max of a sample, the only summary a multi-round figure may be quoted as."""
    return min(xs), statistics.median(xs), max(xs)


def verdict(arm_by_round, ctl_by_round):
    """PAIRED-by-round sign test: does the sign flip inside the run-to-run spread?

    The standing rule is "any comparison whose sign flips inside the run-to-run ranges is
    reported indistinguishable".  Applied to the UNPAIRED min/max that is far too blunt here:
    a busy host puts one or two rounds 3x above the rest, and that single round's spread
    swallows every real effect, so a 235 ns control against a 181 ns arm reads
    `indistinguishable` on nothing but scheduler noise.

    Pairing is what the interleave BOUGHT.  Every arm runs once per round, in rotated order,
    inside the same machine state — so the round is a block, and a round-level disturbance
    moves the control and the arm together.  The test is therefore on the per-round DELTA:
    the effect is real only when `arm - control` carries the same sign in EVERY round, and
    one flip anywhere is `indistinguishable`.  This is strictly more conservative than a
    median comparison and strictly more sensitive than an unpaired range test, and it is the
    reading of the rule the interleave was designed for.
    """
    rounds = sorted(set(arm_by_round) & set(ctl_by_round))
    if not rounds:
        return "indistinguishable", 0.0, []
    deltas = [arm_by_round[r] - ctl_by_round[r] for r in rounds]
    a_med = statistics.median(arm_by_round[r] for r in rounds)
    c_med = statistics.median(ctl_by_round[r] for r in rounds)
    ratio = c_med / a_med if a_med else 0.0
    if all(d < 0 for d in deltas):
        return "FASTER", ratio, deltas
    if all(d > 0 for d in deltas):
        return "SLOWER", ratio, deltas
    return "indistinguishable", ratio, deltas


def main(argv):
    lines = []
    if len(argv) > 1:
        for f in argv[1:]:
            lines += open(f).read().splitlines()
    else:
        lines = sys.stdin.read().splitlines()

    grid = defaultdict(list)       # (arm, payload, segment) -> [p50 per round]
    byround = defaultdict(dict)    # (arm, payload, segment) -> {round: p50}
    reach = defaultdict(lambda: [0, 0])  # (arm, payload, segment) -> [pins, copies]
    ks = {}
    net = defaultdict(list)        # (label, K, payload, slot) -> [(deliv/s, pins, copies, floor, drops)]

    for ln in lines:
        f = ln.split("\t")
        if f and f[0] == "RESULT_PIN" and len(f) >= 12:
            _, _rd, arm, k, payload, segment, p50, _p99, _mean, pins, copies, n = f[:12]
            key = (arm, int(payload), int(segment))
            grid[key].append(int(p50))
            byround[key][int(_rd)] = int(p50)
            reach[key][0] += int(pins)
            reach[key][1] += int(copies)
            ks[arm] = int(k)
        elif f and f[0] == "RESULT_PINNET" and len(f) >= 14:
            (_, label, k, payload, slot, slots, n, rate, pins, copies, floor, drops,
             verts, cap) = f[:14]
            net[(int(verts), label, int(k), int(payload), int(slot), int(cap))].append(
                (float(rate), int(pins), int(copies), int(floor), int(drops), int(n)))

    if grid:
        rounds = max(len(v) for v in grid.values())
        arms = sorted({a for a, _, _ in grid}, key=lambda a: (a != "A-control", a))
        print(f"\n## Store leg — p50 ns, min/median/max across {rounds} interleaved rounds\n")
        print("| payload B | segment B | arm | K | p50 min/med/max ns | pins | copies "
              "| paired delta vs A min/med/max ns | verdict |")
        print("| ---: | ---: | --- | ---: | --- | ---: | ---: | --- | --- |")
        cells = sorted({(p, s) for _, p, s in grid})
        for p, s in cells:
            ctl = grid.get(("A-control", p, s))
            for a in arms:
                v = grid.get((a, p, s))
                if not v:
                    continue
                lo, med, hi = rng(v)
                pins, copies = reach[(a, p, s)]
                seg = "fit" if s == 0 else str(s)
                if a == "A-control" or not ctl:
                    print(f"| {p} | {seg} | {a} | {ks[a]} | {lo}/{med}/{hi} | {pins} "
                          f"| {copies} | -- | control |")
                    continue
                d, ratio, deltas = verdict(byround[(a, p, s)], byround[("A-control", p, s)])
                verd = "indistinguishable" if d == "indistinguishable" else f"{d} {ratio:.2f}x"
                dl, dm, dh = rng(deltas)
                print(f"| {p} | {seg} | {a} | {ks[a]} | {lo}/{med}/{hi} | {pins} | {copies} "
                      f"| {dl:+.0f}/{dm:+.0f}/{dh:+.0f} | {verd} |")

    if net:
        print("\n## Delivered throughput — counted by the RECEIVER process\n")
        print("| vertices | pool slots | arm | K | payload B | slot B "
              "| receiver deliv/s min/med/max | delivered | pins | copies "
              "| free-slot floor | rx drops |")
        print("| ---: | ---: | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |")
        for (verts, label, k, payload, slot, cap), rows in sorted(net.items()):
            rates = [r[0] for r in rows]
            lo, med, hi = rng(rates)
            print(f"| {verts} | {cap} | {label} | {k} | {payload} | {slot} "
                  f"| {lo:.0f}/{med:.0f}/{hi:.0f} "
                  f"| {sum(r[5] for r in rows)} | {sum(r[1] for r in rows)} "
                  f"| {sum(r[2] for r in rows)} | {min(r[3] for r in rows)} "
                  f"| {sum(r[4] for r in rows)} |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
