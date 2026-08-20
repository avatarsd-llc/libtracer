#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# #1266 — the subscriber-index interning A/B, with its own A/A null carried in the same run.
#
# One ROUND runs every arm exactly once, all of them inside ONE process, rotating their own
# order per round (`--round0`). Exhausting one arm's runs before starting the next is the shape
# that produced the recorded 55.2 / 53.0 / 149.8 M deliv/s swing on identical code
# (`bench/run_pin_ratio.sh`), and nothing here does that.
#
# THE A/A NULL IS CARRIED, NOT ASSUMED. Each round runs the SAME binary twice under two tags,
# `A` and `B`, and the tag order alternates by round parity — so the tag sequence across rounds
# is A B / B A / A B / ... , i.e. ABBA-interleaved. `A` and `B` are byte-identical executions;
# any difference between them is the host, not the code. `collate_subscribe_index.py` takes the
# widest per-cell A-vs-B excursion as the null band and writes `within-null` on every arm delta
# smaller than it. A band taken from a different window would not describe THIS window, which is
# why it is collected here rather than quoted from history.
#
#   bash bench/run_subscribe_index.sh <bench-dir> [rounds] [cpu] > raw.tsv
#
# The directory is a built bench/build. Every raw line is emitted verbatim on stdout.
#
# Run it behind `python3 bench/host_guard.py wait --timeout 900` and log `cat /proc/loadavg`
# either side; this bench resolves single-nanosecond differences and a busy neighbour erases
# them. `perf-local` pins the same CPU, so check `gh run list --workflow perf-local.yml` too —
# `host_guard wait` sees load, not intent.
set -euo pipefail

BIN="${1:?bench build dir}"
ROUNDS="${2:-13}"
CPU="${3:-2}"

PIN=(taskset -c "$CPU")

# Reachability before numbers, every run — not once at authoring time.
"${PIN[@]}" "$BIN/bench_subscribe_index" --calibrate || {
    echo "calibration FAILED" >&2
    exit 2
}

# Bytes at rest, once per tag: a footprint is a property of the built structure, not of the
# round, so re-measuring it per round would only add noise to a deterministic number.
for tag in A B; do
    "${PIN[@]}" "$BIN/bench_subscribe_index" --ram --tag="$tag"
done

for ((r = 0; r < ROUNDS; ++r)); do
    if ((r % 2 == 0)); then
        order="A B"
    else
        order="B A"
    fi
    for tag in $order; do
        "${PIN[@]}" "$BIN/bench_subscribe_index" --rounds=1 --round0="$r" --tag="$tag"
    done
done

# Repeat the line-break AFTER the run: an instrument that was live at the start and inert by
# the end would otherwise sign off on every cell in between.
"${PIN[@]}" "$BIN/bench_subscribe_index" --calibrate || {
    echo "POST calibration FAILED" >&2
    exit 2
}
