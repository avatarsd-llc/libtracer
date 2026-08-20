#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# #941 / ADR-0079 §Verification — the per-configuration allocation-store sweep, with its own
# A/A null carried in the same window.
#
# One ROUND runs every arm exactly once, all of them inside ONE process, rotating their own
# order per round (`--round0`). Exhausting one arm's runs before starting the next is the shape
# that produced the recorded 55.2 / 53.0 / 149.8 M deliv/s swing on identical code
# (`bench/run_pin_ratio.sh`), and nothing here does that.
#
# THE A/A NULL IS CARRIED, NOT ASSUMED. Each round runs the SAME binaries twice under two tags,
# `A` and `B`, and the tag order alternates by round parity — so the tag sequence across rounds
# is A B / B A / A B / ... , i.e. ABBA-interleaved. `A` and `B` are byte-identical executions;
# any difference between them is the host, not the code. `collate_store_sweep.py` takes the
# widest per-cell A-vs-B excursion as the null band and writes `within-null` on every arm delta
# smaller than it. A band taken from a different window would not describe THIS window, which is
# why it is collected here rather than quoted from history.
#
#   bash bench/run_store_sweep.sh <bench-dir> [rounds] [cpu] > raw.tsv
#
# The directory is a built bench/build. Every raw line is emitted verbatim on stdout.
#
# TWO MEASUREMENT WINDOWS, AND THEY ARE NOT INTERCHANGEABLE.
#   * `latency`, `hwm` and `escape` run under `taskset -c "$CPU"`, like every other pinned
#     bench here.
#   * `throughput` runs UNPINNED, and must. Its arms sweep T = 1..24 receive threads; pinning a
#     24-thread arm onto one logical CPU would time the scheduler and publish it as allocator
#     contention. The two windows emit under different RESULT tags (`RESULT_STORE_LAT` vs
#     `RESULT_STORE_TPUT`) and `collate_store_sweep.py` keeps them in separate tables so no
#     reader can accidentally compare across them.
#
# Run it behind `python3 bench/host_guard.py wait --timeout 900` and log `cat /proc/loadavg`
# either side (this script does the logging). `perf-local` pins the same CPU, so check
# `gh run list --workflow perf-local.yml` too — `host_guard wait` sees load, not intent. The
# throughput arm wants the WHOLE machine: run it alone (#1369), and hunt orphaned `while :`
# busy-loops first — they survive for days on this host and quietly poison every window.
set -euo pipefail

BIN="${1:?bench build dir}"
ROUNDS="${2:-13}"
CPU="${3:-2}"

PIN=(taskset -c "$CPU")

# Reachability before numbers, every run — not once at authoring time. BOTH binaries: the
# seam-reachability canaries live in the timed harness and the escape / disjointness / null-arm
# canaries live in the census, because only the census overrides `operator new`.
"${PIN[@]}" "$BIN/bench_store_sweep" calibrate || {
    echo "calibration FAILED (sweep)" >&2
    exit 2
}
"${PIN[@]}" "$BIN/bench_store_escape" calibrate || {
    echo "calibration FAILED (escape)" >&2
    exit 2
}

echo "# loadavg before: $(cat /proc/loadavg)"

# Store occupancy is DETERMINISTIC — a function of the workload and the topology, not of the
# machine (the mode runs single-threaded for exactly that reason). Re-measuring it per round
# would add nothing but rows, so it is taken once per tag.
"${PIN[@]}" "$BIN/bench_store_sweep" hwm

for ((r = 0; r < ROUNDS; ++r)); do
    if ((r % 2 == 0)); then
        order="A B"
    else
        order="B A"
    fi
    for tag in $order; do
        "${PIN[@]}" "$BIN/bench_store_sweep" latency --round0="$r" --tag="$tag"
        "${PIN[@]}" "$BIN/bench_store_escape" escape --round0="$r" --tag="$tag"
        # UNPINNED on purpose — see the header.
        "$BIN/bench_store_sweep" throughput --round0="$r" --tag="$tag"
    done
done

echo "# loadavg after: $(cat /proc/loadavg)"

# Repeat the line-break AFTER the run: an instrument that was live at the start and inert by
# the end would otherwise sign off on every cell in between.
"${PIN[@]}" "$BIN/bench_store_sweep" calibrate || {
    echo "POST calibration FAILED (sweep)" >&2
    exit 2
}
"${PIN[@]}" "$BIN/bench_store_escape" calibrate || {
    echo "POST calibration FAILED (escape)" >&2
    exit 2
}
