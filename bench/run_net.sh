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
# Emits RESULT lines on stdout (append to the compare sweep or pipe to render_compare.py);
# diagnostics go to stderr. Never aborts on one failing transport — best-effort.
set -uo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 || true
cmake --build build --target bench_transports -j >/dev/null 2>&1 || true
cmake --build build --target bench_zenoh_net -j >/dev/null 2>&1 || true

# run_pair <binary> <proto> <port>: sub (backgrounded, RESULT -> our stdout) then pub.
run_pair() {
    local bin="$1" proto="$2" port="$3"
    [ -x "$bin" ] || return 0
    timeout 90 "$bin" sub "$proto" "$port" &
    local sub=$!
    sleep 0.7
    timeout 70 "$bin" pub "$proto" "$port" >/dev/null 2>&1 || true
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
