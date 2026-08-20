#!/usr/bin/env python3
"""#1266 collator: the subscriber-index interning curve, read against its OWN A/A null.

SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

Reads `RESULT_SIDX` / `RESULT_SIDX_RAM` / `RESULT_SIDX_DOOR` lines from stdin or files and
renders four tables:

  1. **The A/A null.**  `run_subscribe_index.sh` runs the SAME binary under two tags, `A` and
     `B`, ABBA-interleaved.  The widest per-cell excursion between them is the null band, and
     it is collected in the same window as the result rather than quoted from history —
     a band from another window does not describe this one.
  2. **The curve**, all five link counts at both vertex populations, no cell omitted.  Every
     delta carries a verdict, and a delta smaller than the null band reads `within-null`
     rather than as a win.  That rule is enforced HERE rather than left to the reader.
  3. **Bytes at rest**, on the same axis, with the per-link footprint derived across the link
     sweep at a fixed vertex population.
  4. **The name door**, reported and never gated.  The carry (#1417) trades a hash-and-find
     per SUBSCRIBE for a linear scan per PEER HANGUP; a curve showing only the half that got
     faster would be advocacy, so the half that got slower is printed beside it.

ESTIMATOR: **best-of-rounds**, never median.  `docs/methodology.md` records why — contamination
is one-sided, so the minimum is the estimate least disturbed by it, and the same sample set that
reads -33 %…+54 % by median reads +/-1.44 % by best-of-rounds.  The per-round p50 is the input;
the reported figure is the smallest of them.  The full `[min..max]` spread is printed beside every
figure, because a ~2x spread splitting into two clusters means the window was dirty and the cell
should be re-taken whatever its minimum says.
"""
import sys
import statistics
from collections import defaultdict

#: The arm every index arm's cost is taken net of — the driver loop with no index operation.
kControlArm = "S-sentinel"

#: The live whole-subscribe arm.  Never compared to the control: it does different work.
kLiveArm = "sub-wire"

#: Arms in report order.
kArmOrder = [kControlArm, "idx-string", "idx-token", "idx-token-intern", "idx-token-carry",
             kLiveArm]


def parse(lines):
    """Split the two tags into (latency, ram) dicts keyed by cell."""
    lat = defaultdict(dict)  # (arm, links, verts) -> {(tag, round): p50_ns}
    spread = defaultdict(list)  # (arm, links, verts) -> [p50_ns, ...]
    ram = {}  # (tag, arm, links, verts) -> (live, peak, allocs, entries)
    door = {}  # (tag, arm, links, verts) -> ns per name-door lookup
    for line in lines:
        f = line.rstrip("\n").split("\t")
        if not f:
            continue
        if f[0] == "RESULT_SIDX" and len(f) == 11:
            rnd, tag, arm, links, verts = int(f[1]), f[2], f[3], int(f[4]), int(f[5])
            p50_ns = int(f[6]) / 1000.0  # the bench emits PICOseconds per op
            lat[(arm, links, verts)][(tag, rnd)] = p50_ns
            spread[(arm, links, verts)].append(p50_ns)
        elif f[0] == "RESULT_SIDX_DOOR" and len(f) == 6:
            door[(f[1], f[2], int(f[3]), int(f[4]))] = int(f[5]) / 1000.0
        elif f[0] == "RESULT_SIDX_RAM" and len(f) == 9:
            ram[(f[1], f[2], int(f[3]), int(f[4]))] = (
                int(f[5]),
                int(f[6]),
                int(f[7]),
                int(f[8]),
            )
    return lat, spread, ram, door


def best(samples, tag=None):
    """Best-of-rounds: the smallest per-round p50, optionally restricted to one tag."""
    vals = [v for (t, _r), v in samples.items() if tag is None or t == tag]
    return min(vals) if vals else None


def null_band(lat):
    """Widest per-cell A-vs-B excursion, as a percentage — the band every delta is read against.

    Both tags are the same binary run in the same window, so this is pure host.  Reported as a
    percentage rather than an absolute because the arms differ by 4x in magnitude and an
    absolute band taken off the slowest arm would wave through a real effect on the fastest.
    """
    rows, widest = [], 0.0
    for (arm, links, verts), samples in sorted(lat.items()):
        a, b = best(samples, "A"), best(samples, "B")
        if a is None or b is None:
            continue
        pct = abs(a - b) / min(a, b) * 100.0
        widest = max(widest, pct)
        rows.append((arm, links, verts, a, b, pct))
    return rows, widest


def verdict(delta_ns, reference_ns, band_pct):
    """`within-null` unless the delta exceeds the null band applied to its own reference.

    Note the two figures are built from different round counts, and deliberately so.  The band
    comes from each tag's OWN 13-round best, while the curve's cell values are the best over
    both tags pooled (26 rounds).  A 26-round minimum is less variable than a 13-round one, so
    the true band on the pooled figures is NARROWER than the one applied here — the mismatch
    is conservative, and in the only direction that cannot manufacture a win.
    """
    if reference_ns <= 0:
        return "n/a"
    if abs(delta_ns) < reference_ns * band_pct / 100.0:
        return "within-null"
    return "FASTER" if delta_ns > 0 else "SLOWER"


def main():
    data = sys.stdin if len(sys.argv) < 2 else open(sys.argv[1])
    lat, spread, ram, door = parse(data)
    if not lat:
        print("no RESULT_SIDX rows on input", file=sys.stderr)
        return 2

    rounds = len({r for s in lat.values() for (_t, r) in s})
    print(f"# best-of-rounds over {rounds} rounds x 2 tags; ns per index operation\n")

    null_rows, band = null_band(lat)
    print("## 1. A/A NULL — same binary, two tags, ABBA-interleaved, same window\n")
    print("| arm | links | verts | A (ns) | B (ns) | excursion |")
    print("|---|---:|---:|---:|---:|---:|")
    for arm, links, verts, a, b, pct in null_rows:
        print(f"| {arm} | {links} | {verts} | {a:.3f} | {b:.3f} | {pct:.2f}% |")
    print(f"\n**NULL BAND = {band:.2f}%** (widest per-cell A-vs-B excursion). "
          "Any delta below this is reported `within-null`, never as a win.\n")

    print("## 2. THE CURVE — all five link counts, both vertex populations\n")
    print("| verts | links | sentinel | idx-string | idx-token | idx-token-intern "
          "| idx-token-carry | sub-wire | string-token (ceiling) | verdict "
          "| string-carry (SHIPPED) | verdict | string-intern | verdict |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---|---:|---|")
    verts_seen = sorted({v for (_a, _l, v) in lat})
    links_seen = sorted({l for (_a, l, _v) in lat})
    for verts in verts_seen:
        for links in links_seen:
            def cell(arm):
                s = lat.get((arm, links, verts))
                return best(s) if s else None

            ctl = cell(kControlArm)
            s_str, s_tok = cell("idx-string"), cell("idx-token")
            s_int, live = cell("idx-token-intern"), cell(kLiveArm)
            s_car = cell("idx-token-carry")
            if None in (ctl, s_str, s_tok, s_int, s_car):
                continue
            # Net of the control: the driver loop and the clock are the same in every arm.
            n_str, n_tok, n_int = s_str - ctl, s_tok - ctl, s_int - ctl
            n_car = s_car - ctl
            d_ceiling = s_str - s_tok
            d_carry = s_str - s_car
            d_intern = s_str - s_int
            live_s = f"{live:.0f}" if live else "-"
            print(f"| {verts} | {links} | {ctl:.2f} | {s_str:.2f} ({n_str:+.2f}) | "
                  f"{s_tok:.2f} ({n_tok:+.2f}) | {s_int:.2f} ({n_int:+.2f}) | "
                  f"{s_car:.2f} ({n_car:+.2f}) | {live_s} | "
                  f"{d_ceiling:+.2f} | {verdict(d_ceiling, s_str, band)} | "
                  f"{d_carry:+.2f} | {verdict(d_carry, s_str, band)} | "
                  f"{d_intern:+.2f} | {verdict(d_intern, s_str, band)} |")
    print("\n`idx-token-carry` is the SHIPPED shape and pays for the token; `idx-token` charges "
          "nothing for it and is a CEILING, not a forecast. `idx-string` is what the index was "
          "before the carry landed, and is the column every saving is quoted against.\n")
    print("Figures in parentheses are net of the `S-sentinel` control. `sub-wire` is the LIVE "
          "`graph_t::subscribe_wire` cost of one whole remote subscribe, in ns; it is not "
          "comparable to the control and is printed for proportion only.\n")

    print("## 3. SPREAD — every cell's [min..max] across rounds\n")
    print("| arm | links | verts | min | median | max | max/min |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for (arm, links, verts), vals in sorted(spread.items()):
        lo, hi = min(vals), max(vals)
        print(f"| {arm} | {links} | {verts} | {lo:.2f} | {statistics.median(vals):.2f} | "
              f"{hi:.2f} | {hi / lo:.2f}x |")

    if door:
        print("\n## 5. THE NAME DOOR — reported, never gated\n")
        print("The carry trades a hash-and-find per SUBSCRIBE for a linear scan per PEER "
              "HANGUP. This is the half that got slower, at the rate it actually runs.\n")
        print("| links | verts | idx-string (ns) | idx-token-carry (ns) | ratio |")
        print("|---:|---:|---:|---:|---:|")
        for (tag, arm, links, verts), ns in sorted(door.items()):
            if tag != "A" or arm != "idx-string":
                continue
            car = door.get(("A", "idx-token-carry", links, verts))
            if car is None:
                continue
            print(f"| {links} | {verts} | {ns:.1f} | {car:.1f} | {car / ns:.2f}x |")

    if ram:
        print("\n## 4. BYTES AT REST\n")
        print("| arm | links | verts | live B | peak B | allocs | candidates/link |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for (tag, arm, links, verts), (live, peak, allocs, ents) in sorted(ram.items()):
            if tag != "A":
                continue  # both tags are the same binary; the footprint is deterministic
            print(f"| {arm} | {links} | {verts} | {live} | {peak} | {allocs} | {ents} |")
        print("\n### Per-link footprint — (bytes@65 - bytes@4) / 61, vertex set held constant\n")
        print("| arm | verts | bytes@4 | bytes@65 | B per link |")
        print("|---|---:|---:|---:|---:|")
        for arm in kArmOrder:
            for verts in verts_seen:
                lo = ram.get(("A", arm, 4, verts))
                hi = ram.get(("A", arm, 65, verts))
                if not lo or not hi:
                    continue
                print(f"| {arm} | {verts} | {lo[0]} | {hi[0]} | "
                      f"{(hi[0] - lo[0]) / 61.0:.1f} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
