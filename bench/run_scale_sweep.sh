#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# #1485 — drive bench_scale_sweep over the population ladder, ONE POPULATION PER PROCESS.
#
# Why one process per N, when every arm deliberately shares one process at a given N: the RSS
# column. A population's pages go back to the allocator on teardown, not to the kernel, so the
# second population in a process builds out of pages the process already holds and its RSS delta
# reads ~0 — a fact about malloc's arenas, not about a vertex. `heap_per_vertex` (mallinfo2) is
# valid at every position; `rss_per_vertex` is only valid for the first. Exec'ing per N makes
# every N the first.
#
# The arms at a GIVEN N still all share one process, which is the part that matters for the
# comparison: this host's cross-build layout sensitivity is up to +9.8% on an untouched leg
# (see bench/README.md), larger than part of the effect being decomposed.
#
# Host discipline: this refuses to run on a busy box. Absolutes here are only meaningful with the
# load recorded beside them — the same gated point read 3.6 M/s loaded and 6.0 M/s quiet on this
# machine, a factor of 1.6 — and the five self-hosted CI runners live here, so a quiet-looking
# moment can still be a compile fleet. The load actually measured under is echoed into the
# output so a reader never has to take it on trust.
#
# usage: ./run_scale_sweep.sh [ladder]        (default 1000,10000,100000,1000000)
set -euo pipefail
cd "$(dirname "$0")"

LADDER="${1:-1000,10000,100000,1000000}"
ROUNDS="${SCALE_ROUNDS:-3}"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j --target bench_scale_sweep >/dev/null

load1="$(cut -d' ' -f1 /proc/loadavg)"
compilers="$(pgrep -c cc1plus || true)"
echo "HOST loadavg=$(cat /proc/loadavg) cc1plus=${compilers:-0} nproc=$(nproc)"
if [ "${SCALE_FORCE:-0}" != "1" ]; then
    if [ "${compilers:-0}" -gt 0 ]; then
        echo "refusing: ${compilers} cc1plus processes are running — a compile fleet is on this box." >&2
        echo "  (set SCALE_FORCE=1 to measure anyway; the numbers will not be absolutes.)" >&2
        exit 1
    fi
    if [ "$(printf '%.0f' "$load1")" -gt 4 ]; then
        echo "refusing: 1-minute load average is ${load1}." >&2
        echo "  Tell-tale of an orphaned perf-fork busy loop is high load with an idle %Cpu." >&2
        echo "  (set SCALE_FORCE=1 to measure anyway; the numbers will not be absolutes.)" >&2
        exit 1
    fi
fi

IFS=',' read -r -a pops <<<"$LADDER"
for n in "${pops[@]}"; do
    SCALE_LADDER="$n" SCALE_ROUNDS="$ROUNDS" ./build/bench_scale_sweep
done

echo "HOST-AFTER loadavg=$(cat /proc/loadavg) cc1plus=$(pgrep -c cc1plus || echo 0)"
