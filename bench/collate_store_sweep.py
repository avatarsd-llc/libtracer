#!/usr/bin/env python3
"""#941 collator: the ADR-0079 per-configuration store sweep, read against its OWN A/A null.

SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

Reads `RESULT_STORE_LAT` / `RESULT_STORE_TPUT` / `RESULT_STORE_HWM` / `RESULT_STORE_CHAN` /
`RESULT_STORE_ESCAPE` lines from stdin or files and renders the matrix ADR-0079 §Verification
commissions: per configuration, alloc hot-path latency, fan-out throughput at increasing thread
counts, and memory high-water.

ESTIMATOR: **best-of-rounds**, never median.  `docs/methodology.md` makes that normative —
contamination is one-sided, so the minimum is the estimate least disturbed by it, and the same
sample set that reads -33 %…+54 % by median reads +/-1.44 % by best-of-rounds.  The full
`[min..max]` spread is printed beside every figure, because a ~2x spread splitting into two
clusters means the window was dirty and the cell should be re-taken whatever its minimum says.

THE COMPARISON RULES ARE ENFORCED HERE, NOT LEFT TO THE READER.  Two of them, because the two
timed windows have different structure:

  * **Latency** — `null_band()` takes the widest per-cell A-vs-B excursion as a PERCENTAGE and
    `verdict()` prints `within-null` for any delta below it.  A percentage rather than an
    absolute: the legs differ by more than 4x in magnitude, and an absolute band taken off the
    slowest would wave through a real effect on the fastest.  This is
    `collate_subscribe_index.py`'s rule verbatim.
  * **Throughput** — the paired per-round sign test of `collate_pin.py::verdict()`.  Every arm
    runs once per round in rotated order inside the same machine state, so the round is a
    block; the effect is real only when `arm - control` carries the same sign in EVERY round,
    and one flip anywhere prints `indistinguishable`.

TWO WINDOWS, KEPT APART.  The latency / hwm / escape rows are taken under `taskset -c <cpu>`;
the throughput rows are taken UNPINNED, because a 24-thread arm on one logical CPU measures
nothing.  They are never compared to each other and are rendered in separate tables.

HOW MUCH EACH MEMORY COLUMN IS WORTH, in descending order of trust:
  1. **Store `used()`** (`RESULT_STORE_HWM`) — DETERMINISTIC.  Taken single-threaded, so it is a
     function of the workload and the topology only.  This is the column a deployer sizes a slab
     against, and the only memory column that is a candidate for a CI gate.
  2. **Per-channel draw counts** (`RESULT_STORE_CHAN`) — also deterministic; they say which
     channel drew what, and a zero here means an arm measured nothing at that seam.
  3. **Process-heap escape** (`RESULT_STORE_ESCAPE`) — NON-deterministic.
     `bench/ram_census_pins.json` records a transport high-water moving 66 % across runs and
     ~41 KB within one run, which is why `ram_census.py` excludes high-water columns from its
     pinned rows.  Reported best-of-rounds with its spread, and NEVER gated.
"""
import statistics
import sys
from collections import defaultdict

#: The arm every other arm's delta is taken against — today's shipped default.
kControlArm = "H-baseline"

#: Arms in report order, control first.
kArmOrder = [kControlArm, "WIDE", "MID", "NARROW"]

#: Latency legs in report order.  `full` is the composite the throughput arm runs.
kLegOrder = ["net-fwd", "graph-write", "graph-read", "full"]


def parse(lines):
    """Split the five tags into the dicts each table needs."""
    lat = defaultdict(dict)       # (arm, leg) -> {(tag, round): p50_ns}
    lat_spread = defaultdict(list)
    tput = defaultdict(dict)      # (arm, leg, T) -> {(tag, round): per_thread_ops_s}
    tput_spread = defaultdict(list)
    hwm = {}                      # (arm, T, store, idx) -> (used, cap, classes, overflow)
    chan = {}                     # (arm, T, channel) -> (blocks, bytes, peak, refusals)
    esc = defaultdict(dict)       # (arm, window) -> {(tag, round): (live, peak, allocs, frees)}
    for line in lines:
        f = line.rstrip("\n").split("\t")
        if not f:
            continue
        if f[0] == "RESULT_STORE_LAT" and len(f) == 10:
            rnd, tag, arm, leg = int(f[1]), f[2], f[3], f[4]
            p50_ns = int(f[5]) / 1000.0  # the bench emits PICOseconds per op
            lat[(arm, leg)][(tag, rnd)] = p50_ns
            lat_spread[(arm, leg)].append(p50_ns)
        elif f[0] == "RESULT_STORE_TPUT" and len(f) == 9:
            rnd, tag, arm, leg, T = int(f[1]), f[2], f[3], f[4], int(f[5])
            per_thread = float(f[6])
            tput[(arm, leg, T)][(tag, rnd)] = per_thread
            tput_spread[(arm, leg, T)].append(per_thread)
        elif f[0] == "RESULT_STORE_HWM" and len(f) == 9:
            hwm[(f[1], int(f[2]), f[3], int(f[4]))] = (
                int(f[5]), int(f[6]), int(f[7]), int(f[8]))
        elif f[0] == "RESULT_STORE_CHAN" and len(f) == 8:
            chan[(f[1], int(f[2]), f[3])] = (int(f[4]), int(f[5]), int(f[6]), int(f[7]))
        elif f[0] == "RESULT_STORE_ESCAPE" and len(f) == 9:
            rnd, tag, arm, window = int(f[1]), f[2], f[3], f[4]
            esc[(arm, window)][(tag, rnd)] = (int(f[5]), int(f[6]), int(f[7]), int(f[8]))
    return lat, lat_spread, tput, tput_spread, hwm, chan, esc


def best(samples, tag=None, largest=False):
    """Best-of-rounds, optionally restricted to one tag.

    `largest=True` for a RATE (more is better); the default takes the smallest, which is the
    right end for a LATENCY.  Both are "the round least disturbed by the host", which is what
    the estimator is for — contamination only ever moves a latency up and a rate down.
    """
    vals = [v for (t, _r), v in samples.items() if tag is None or t == tag]
    if not vals:
        return None
    return max(vals) if largest else min(vals)


def null_band(cells, largest=False):
    """Widest per-cell A-vs-B excursion, as a percentage — the band every delta is read against.

    Both tags are the same binary run in the same window, so this is pure host.  Reported as a
    percentage rather than an absolute because the legs differ by 4x in magnitude and an
    absolute band taken off the slowest leg would wave through a real effect on the fastest.
    """
    rows, widest = [], 0.0
    for key, samples in sorted(cells.items()):
        a, b = best(samples, "A", largest), best(samples, "B", largest)
        if a is None or b is None or min(a, b) <= 0:
            continue
        pct = abs(a - b) / min(a, b) * 100.0
        widest = max(widest, pct)
        rows.append((key, a, b, pct))
    return rows, widest


def verdict(delta, reference, band_pct, faster_is_positive=True):
    """`within-null` unless the delta exceeds the null band applied to its own reference.

    The band comes from each tag's OWN best over N rounds, while the cell value is the best over
    both tags pooled (2N rounds).  A 2N-round minimum is less variable than an N-round one, so
    the true band on the pooled figures is NARROWER than the one applied here — the mismatch is
    conservative, and in the only direction that cannot manufacture a win.
    """
    if reference <= 0:
        return "n/a"
    if abs(delta) < reference * band_pct / 100.0:
        return "within-null"
    good = delta > 0 if faster_is_positive else delta < 0
    return "FASTER" if good else "SLOWER"


def sign_test(arm_by_round, ctl_by_round):
    """PAIRED-by-round sign test — `collate_pin.py::verdict()`'s rule, for the rate arm.

    Pairing is what the interleave BOUGHT.  Every arm runs once per round, in rotated order,
    inside the same machine state, so a round-level disturbance moves the control and the arm
    together and the test can be on the per-round DELTA.  One sign flip anywhere is
    `indistinguishable`, and no amount of median separation upgrades that.  Applied to a RATE,
    so a positive delta is the arm being faster.
    """
    rounds = sorted(set(arm_by_round) & set(ctl_by_round))
    if not rounds:
        return "indistinguishable", 0.0
    deltas = [arm_by_round[r] - ctl_by_round[r] for r in rounds]
    a_med = statistics.median(arm_by_round[r] for r in rounds)
    c_med = statistics.median(ctl_by_round[r] for r in rounds)
    ratio = a_med / c_med if c_med else 0.0
    if all(d > 0 for d in deltas):
        return "FASTER", ratio
    if all(d < 0 for d in deltas):
        return "SLOWER", ratio
    return "indistinguishable", ratio


def by_round(samples):
    """Collapse the (tag, round) key to a round key, keeping the better of the two tags."""
    out = {}
    for (_tag, rnd), v in samples.items():
        out[rnd] = v if rnd not in out else min(out[rnd], v)
    return out


def by_round_max(samples):
    """@see by_round — the rate flavour, keeping the larger of the two tags."""
    out = {}
    for (_tag, rnd), v in samples.items():
        out[rnd] = v if rnd not in out else max(out[rnd], v)
    return out


def table_null(lat, tput):
    """Table 1 — the A/A null, collected in THIS window, and the two bands it yields."""
    lat_rows, lat_band = null_band(lat)
    tp_rows, tp_band = null_band(tput, largest=True)

    print("## 1. A/A NULL — same binaries, two tags, ABBA-interleaved, same window\n")
    print("### 1a. Latency window (pinned)\n")
    print("| arm | leg | A (ns) | B (ns) | excursion |")
    print("|---|---|---:|---:|---:|")
    for (arm, leg), a, b, pct in lat_rows:
        print(f"| {arm} | {leg} | {a:.2f} | {b:.2f} | {pct:.2f}% |")
    print(f"\n**LATENCY NULL BAND = {lat_band:.2f}%** (widest per-cell A-vs-B excursion). "
          "Any delta below this is reported `within-null`, never as a win.\n")

    print("### 1b. Throughput window (UNPINNED — a different window, its own band)\n")
    print("| arm | leg | T | A (ops/s/thread) | B (ops/s/thread) | excursion |")
    print("|---|---|---:|---:|---:|---:|")
    for (arm, leg, T), a, b, pct in tp_rows:
        print(f"| {arm} | {leg} | {T} | {a:,.0f} | {b:,.0f} | {pct:.2f}% |")
    print(f"\n**THROUGHPUT NULL BAND = {tp_band:.2f}%.** The throughput verdicts below do NOT "
          "use this band — they use the paired per-round sign test, which is strictly more "
          "conservative on a blocked design. The band is printed so the reader can see how "
          "much of the machine is in an unpinned 24-thread window.\n")
    return lat_band, tp_band


def table_latency(lat, band):
    """Table 2 — per-configuration latency, best-of-rounds, paired against the control."""
    print("## 2. LATENCY — best-of-rounds ns per operation, by workload leg\n")
    print("Legs, and the ADR-0079 channel each one draws: `net-fwd` = the rope FWD forward hop "
          "(router `rx` + the transport egress gather); `graph-write` = the `std::pmr` `mr` "
          "channel; `graph-read` = the composed-subtree `ctl` channel; `full` = all three, "
          "which is what the throughput arm runs.\n")
    print("| leg | arm | ns/op | delta vs control | verdict |")
    print("|---|---|---:|---:|---|")
    for leg in kLegOrder:
        ctl = best(lat.get((kControlArm, leg), {}))
        for arm in kArmOrder:
            v = best(lat.get((arm, leg), {}))
            if v is None:
                continue
            if arm == kControlArm or ctl is None:
                print(f"| {leg} | {arm} | {v:.2f} | -- | control |")
                continue
            d = ctl - v  # positive => the arm is faster than the control
            print(f"| {leg} | {arm} | {v:.2f} | {d:+.2f} | {verdict(d, ctl, band)} |")
    print()


def table_throughput(tput):
    """Table 3 — the fan-out arm, with the paired sign test as its verdict."""
    print("## 3. FAN-OUT THROUGHPUT — best-of-rounds ops/s PER THREAD, unpinned window\n")
    legs = sorted({leg for (_a, leg, _t) in tput})
    for leg in legs:
        Ts = sorted({T for (_a, lg, T) in tput if lg == leg})
        print(f"### leg `{leg}`\n")
        print("| arm | " + " | ".join(f"T={T}" for T in Ts) +
              " | T=1 -> T=max | verdict at T=max vs control |")
        print("|---" * (len(Ts) + 3) + "|")
        for arm in kArmOrder:
            cells = []
            for T in Ts:
                v = best(tput.get((arm, leg, T), {}), largest=True)
                cells.append(f"{v:,.0f}" if v else "-")
            first = best(tput.get((arm, leg, Ts[0]), {}), largest=True)
            last = best(tput.get((arm, leg, Ts[-1]), {}), largest=True)
            scale = f"{last / first:.2f}x" if first and last else "-"
            if arm == kControlArm:
                verd = "control"
            else:
                a = by_round_max(tput.get((arm, leg, Ts[-1]), {}))
                c = by_round_max(tput.get((kControlArm, leg, Ts[-1]), {}))
                d, ratio = sign_test(a, c)
                verd = d if d == "indistinguishable" else f"{d} {ratio:.2f}x"
            print(f"| {arm} | " + " | ".join(cells) + f" | {scale} | {verd} |")
        print()


def table_memory(hwm, chan, esc):
    """Table 4 — the memory column, in descending order of how much it can be trusted."""
    print("## 4. MEMORY\n")

    if hwm:
        print("### 4a. Store high-water — `pool_source_t::used()`. DETERMINISTIC.\n")
        print("Taken single-threaded over all T lanes, so it is a function of the workload and "
              "the topology and not of the scheduler. This is the column a deployer sizes a "
              "slab against.\n")
        print("| arm | T | total used B | stores | per-store used (B) | classes | overflow |")
        print("|---|---:|---:|---:|---|---:|---:|")
        arms = sorted({a for (a, _t, _s, _i) in hwm}, key=lambda a: kArmOrder.index(a)
                      if a in kArmOrder else 99)
        Ts = sorted({T for (_a, T, _s, _i) in hwm})
        for arm in arms:
            for T in Ts:
                rows = [(k, v) for k, v in hwm.items() if k[0] == arm and k[1] == T]
                if not rows:
                    continue
                total = sum(v[0] for _k, v in rows)
                classes = sum(v[2] for _k, v in rows)
                over = sum(v[3] for _k, v in rows)
                per = {}
                for (_a, _T, store, _i), v in rows:
                    per.setdefault(store, []).append(v[0])
                detail = ", ".join(f"{s}: {min(vs)}..{max(vs)} x{len(vs)}"
                                   for s, vs in sorted(per.items()))
                print(f"| {arm} | {T} | {total:,} | {len(rows)} | {detail} | {classes} "
                      f"| {over} |")
        print("\n`H-baseline` has no rows here: it injects no store, so there is nothing to "
              "measure the occupancy of. Its cost is the escape column below, in full.\n")

    if chan:
        print("### 4b. Per-channel draw — which of ADR-0079's four channels drew what. "
              "DETERMINISTIC.\n")
        print("A ZERO in `blocks` is not a good result, it is a VACUOUS arm: the channel was "
              "wired to a store nothing draws from. `bench_store_sweep calibrate` gates on "
              "exactly that.\n")
        print("| arm | T | channel | blocks | bytes | peak live B | refusals |")
        print("|---|---:|---|---:|---:|---:|---:|")
        for (arm, T, ch), (blocks, byts, peak, ref) in sorted(chan.items()):
            print(f"| {arm} | {T} | {ch} | {blocks:,} | {byts:,} | {peak:,} | {ref} |")
        print()

    if esc:
        print("### 4c. Process-heap ESCAPE — what each composition still takes from the global "
              "heap. NON-deterministic; NEVER gated.\n")
        print("`build` = standing the node up, with the injected slabs allocated BEFORE the "
              "counter armed, so this is what the node reached for BEYOND its own budget. "
              "`steady` = one workload iteration's worth, after the build window closed. "
              "`bench/ram_census_pins.json` records a transport high-water moving 66% across "
              "runs, which is why the peak column carries its spread and is not a gate "
              "candidate.\n")
        print("| arm | window | live delta B | peak delta B [min..max] | allocs | frees |")
        print("|---|---|---:|---|---:|---:|")
        for (arm, window) in sorted(esc, key=lambda k: (kArmOrder.index(k[0])
                                                        if k[0] in kArmOrder else 99, k[1])):
            samples = esc[(arm, window)]
            lives = [v[0] for v in samples.values()]
            peaks = [v[1] for v in samples.values()]
            allocs = [v[2] for v in samples.values()]
            frees = [v[3] for v in samples.values()]
            print(f"| {arm} | {window} | {min(lives):,} | {min(peaks):,}..{max(peaks):,} "
                  f"| {min(allocs):,} | {min(frees):,} |")
        print("\n**The escape is not zero in ANY arm, and cannot be.** `vertex_t` placement is "
              "still `std::make_unique<vertex_t>` on the global heap — ADR-0079 Stage 2 "
              "(#843, gated on #1285), not landed. This sweep varies descent stacks and the "
              "`std::pmr` control-block channel, not vertex placement. That number IS the "
              "substrate train's closing evidence about what \"bounded node\" currently "
              "delivers.\n")


def table_spread(lat_spread, tput_spread):
    """Table 5 — every cell's spread, printed whatever the estimator said."""
    print("## 5. SPREAD — every cell's [min..max] across rounds\n")
    print("A ~2x spread that splits into two clusters means the window was dirty and the cell "
          "should be re-taken whatever its minimum says.\n")
    print("| window | cell | min | median | max | max/min |")
    print("|---|---|---:|---:|---:|---:|")
    for (arm, leg), vals in sorted(lat_spread.items()):
        lo, hi = min(vals), max(vals)
        print(f"| latency (ns) | {arm}/{leg} | {lo:.2f} | {statistics.median(vals):.2f} "
              f"| {hi:.2f} | {hi / lo:.2f}x |")
    for (arm, leg, T), vals in sorted(tput_spread.items()):
        lo, hi = min(vals), max(vals)
        ratio = hi / lo if lo > 0 else 0.0
        print(f"| throughput (ops/s/thread) | {arm}/{leg}/T={T} | {lo:,.0f} "
              f"| {statistics.median(vals):,.0f} | {hi:,.0f} | {ratio:.2f}x |")
    print()


def main(argv):
    if len(argv) > 1:
        lines = []
        for f in argv[1:]:
            lines += open(f).read().splitlines()
    else:
        lines = sys.stdin.read().splitlines()

    lat, lat_spread, tput, tput_spread, hwm, chan, esc = parse(lines)
    if not lat and not tput:
        print("no RESULT_STORE_* rows on input", file=sys.stderr)
        return 2

    rounds = len({r for s in lat.values() for (_t, r) in s})
    print(f"# ADR-0079 per-configuration store sweep (#941) — best-of-rounds over {rounds} "
          "rounds x 2 tags\n")
    lat_band, _tp_band = table_null(lat, tput)
    table_latency(lat, lat_band)
    table_throughput(tput)
    table_memory(hwm, chan, esc)
    table_spread(lat_spread, tput_spread)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
