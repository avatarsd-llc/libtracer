#!/usr/bin/env python3
"""@brief Reduce several ROUNDS of bench output to one best-of-rounds stream.

Reads the concatenated stdout of N executions of the bench binaries and writes a single
RESULT stream in which every point carries its BEST observation across those rounds:
the highest throughput and the lowest latency it was seen to reach.

@section why_best Why best-of-rounds, and never median-of-rounds

Contamination on a shared bench host is ONE-SIDED. A neighbouring build can only ever
make a round slower — nothing on the machine can make a point publish faster than the
code allows — so the round-to-round spread is not noise around a true value, it is a
one-tailed pile of penalties hanging off it. The best round is therefore the estimator
closest to what the code does; the median is an estimate of how busy the host was, and it
moves with the neighbours rather than with the software. Measured here: a median-of-rounds
reduction turned an instrument that agreed with itself to +/-0.34% into one that read
anywhere from -33% to +54% on unchanged code.

@section why_symmetric Why this runs over BOTH engines

This is the reduction for the published libtracer-vs-Zenoh comparison, and the reason it
takes the whole stream rather than one engine's rows is that applying best-of-rounds to
one arm and a single round to the other is not a measurement, it is a thumb on the scale:
the arm that gets N tries keeps its luckiest round while the arm that gets one keeps
whatever the host did to it. Both engines are executed the same number of times, in the
same rounds, and reduced by this one pass, so neither can pick up an advantage the other
was not offered. `RESULT` rows from any `system` value are reduced identically -- there is
deliberately no per-engine branch in this file to get wrong.

@section per_metric Best is taken PER METRIC, matching perf_gate.py

`perf_gate.py`'s `best_of` keeps `min p50`, `min mean` and `max deliv_s` independently for
a point rather than electing one winning round and copying its whole row, and this file
follows it so the comparison charts and the regression gate mean the same thing by "best".
A row emitted here can therefore mix columns from different rounds. That is intended: each
column is a separate one-tailed estimate, and taking the best of each is what removes the
host from all of them.

@section tail RESULT_TAIL is passed through, NOT reduced

Tail rows keep their FIRST observation per key and are not best-of-ed. A p999 is published
on the performance page as the jitter floor a consumer on this topology inherits, and the
minimum p999 across rounds is the opposite of that quantity -- it would advertise the
calmest moment the host ever had as though it were the worst case. The tail is documented
there as published-not-gated for exactly this reason, so it is left as measured.

Usage:
    cat round1.txt round2.txt round3.txt | ./best_of_rounds.py > best.txt
"""
import sys

RESULT_FIELDS = 12

# Column index -> reducer, for RESULT lines. Throughput columns take the maximum and
# latency columns the minimum; both are "the best this point was seen to do".
MAX_COLS = (6, 7, 8)  # pub/s, deliveries/s, MB/s
MIN_COLS = (9, 10, 11)  # p50 ns, p99 ns, mean ns


def key_of(f: list[str]) -> tuple:
    """@brief The (system, mode, size, fanout, endpoints) tuple a point is identified by."""
    return tuple(f[1:6])


def main() -> int:
    """@brief Fold stdin's rounds into one best-of stream on stdout, preserving first-seen order."""
    best: dict[tuple, list[str]] = {}
    tails: dict[tuple, list[str]] = {}
    order: list[tuple[str, tuple]] = []
    passthrough: list[str] = []

    for line in sys.stdin:
        f = line.rstrip("\n").split("\t")
        if len(f) != RESULT_FIELDS or f[0] not in ("RESULT", "RESULT_TAIL"):
            # Anything that is not a point row (banners, warnings) rides through once.
            if line.strip() and line not in passthrough:
                passthrough.append(line)
            continue
        k = key_of(f)
        if f[0] == "RESULT_TAIL":
            if k not in tails:
                tails[k] = f
                order.append(("RESULT_TAIL", k))
            continue
        if k not in best:
            best[k] = f
            order.append(("RESULT", k))
            continue
        cur = best[k]
        for i in MAX_COLS:
            if float(f[i]) > float(cur[i]):
                cur[i] = f[i]
        for i in MIN_COLS:
            if int(f[i]) < int(cur[i]):
                cur[i] = f[i]

    for tag, k in order:
        row = best[k] if tag == "RESULT" else tails[k]
        sys.stdout.write("\t".join(row) + "\n")
    for line in passthrough:
        sys.stderr.write(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
