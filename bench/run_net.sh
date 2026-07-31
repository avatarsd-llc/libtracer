#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Two-process NETWORK transport comparison: libtracer vs Zenoh over the real loopback
# kernel path, for each transport (UDP / TCP / WebSocket). For every (engine, protocol)
# it launches a separate subscriber and publisher process — the SAME two-process topology
# for both engines, so the comparison is fair — and the subscriber emits the mode-tagged
# `net-<proto>` RESULT line to stdout. Vendor Zenoh first with ./fetch_zenoh.sh.
#
# Emits RESULT lines on stdout, each followed by its RESULT_TAIL companion (p999, worst
# sample, sample count, adequacy flag) — append to the compare sweep or pipe to
# render_compare.py. Diagnostics go to stderr. Never aborts on one failing transport.
set -uo pipefail
cd "$(dirname "$0")"

# Paced latency probes per payload size. The bench's own default is 4000, which is BELOW
# bench::kTailSampleFloor: at n=4000 only three samples sit above the p999, so the figure
# is one of the top few draws rather than an estimate of a tail, the emitter reports
# tail_ok=0, and render_compare.py withholds the p999 chart. This is the run that
# PUBLISHES those numbers, so it is the run that has to pay for them.
#
# The price is small and linear: at the 150 us pacing interval each extra 1000 probes is
# 0.15 s per payload size, so 4000 -> 10000 costs +0.9 s per size, +3.6 s per publisher
# process, and about +15 s across the four (engine x protocol) pairs this script drives.
# The `timeout`s below are raised to match with the same headroom they had before.
# Override for a deeper tail — 100000 puts 99 samples above the p999 — but raise the
# timeouts alongside it or the publisher is killed mid-sweep and the last size reports
# nothing at all.
: "${LIBTRACER_BENCH_LAT_MSGS:=10000}"
export LIBTRACER_BENCH_LAT_MSGS

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 || true
cmake --build build --target bench_transports -j >/dev/null 2>&1 || true
cmake --build build --target bench_zenoh_net -j >/dev/null 2>&1 || true

# run_pair <binary> <proto> <port>: sub (backgrounded, RESULT -> our stdout) then pub.
run_pair() {
    local bin="$1" proto="$2" port="$3"
    [ -x "$bin" ] || return 0
    timeout 150 "$bin" sub "$proto" "$port" &
    local sub=$!
    sleep 0.7
    timeout 130 "$bin" pub "$proto" "$port" >/dev/null 2>&1 || true
    wait "$sub" 2>/dev/null || true
}

lport=48400
zport=48500
# UDP + TCP only: both stable and reproducible over the loopback path. WebSocket is
# built and works for throughput, but libtracer's WS transport shows large single-run
# latency spikes (order-of-magnitude p50 swings) that make a published latency chart
# misleading — held until that jitter is understood. QUIC needs the -DLIBTRACER_WITH_QUIC
# module (msquic + TLS), gated like the dedicated quic CI job.
for proto in udp tcp; do
    echo "[libtracer] net-$proto (sub/pub :$lport)" >&2
    run_pair ./build/bench_transports "$proto" "$lport"
    if [ -x ./build/bench_zenoh_net ]; then
        echo "[zenoh] net-$proto (sub/pub :$zport)" >&2
        run_pair ./build/bench_zenoh_net "$proto" "$zport"
    else
        echo "(zenoh not vendored — run ./fetch_zenoh.sh for the comparison)" >&2
    fi
    lport=$((lport + 10))
    zport=$((zport + 10))
done
