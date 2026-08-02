#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# RFC-0022 §6, host half — the two-process, delivery-COUNTED round runner.
#
# Each arm is one (subscriber, publisher) process pair over real loopback UDP; the number that
# leaves this script is the count the SUBSCRIBER observed landing in its graph, never the
# publisher's send rate. Arms rotate per round, for the same reason as run_pin_ratio.sh.
#
# THE CONTROL ARM IS B-sentinel, run from the same binary at K = 0 (`tr::graph::kPinNever`).
# It used to be a second binary built against untouched origin/main; RFC-0022 §3.B deleted
# `settings_t`, so that build no longer exists and cannot be recreated. What B-sentinel
# controls for is the one-copy store branch on the same binary, pool and transport as every
# pinning arm.
#
# The subscriber's RX backend is a bounded pool, so a pinned value holds a whole RX SLOT for
# its lifetime and `available()` becomes a measurable free-slot floor. That is the only lever
# that makes §3.D reachable at a defensible K at all: on the default heap backend the RX
# segment is `kMaxDatagram` (65,536 B) whatever the datagram's length, so a 1 KB payload needs
# K >= 64 before it pins.
#
# VERTICES is the RAM axis. A single STORED_VALUE vertex holds one value, so pinning it holds
# one RX slot however big the segment; the held quantity is `live vertices x segment_bytes`.
# Sweeping the vertex count across the slot count is what turns "pinning costs RAM" from a
# definition into an observable: below the slot count the pool absorbs it, above it the
# transport must refuse datagrams, and `dropped_rx` says when.
#
#   run_pin_net.sh <bench-dir> [rounds] [payload] [slot] [slots]
#
# Env: VERTEX_SET (space-separated), COUNT, WINDOW_MS.
set -euo pipefail

BIN="${1:?bench build dir}"
ROUNDS="${2:-10}"
PAYLOAD="${3:-1024}"
SLOT="${4:-2048}"
SLOTS="${5:-64}"
COUNT="${COUNT:-200000}"
WINDOW_MS="${WINDOW_MS:-6000}"

# arm : K
ARMS=(
    "B-sentinel:0"
    "D2:2"
    "D4:4"
    "D8:8"
    "C-pin-always:4294967295"
)

VERTEX_SET="${VERTEX_SET:-1 8 32 128}"

port=47300
for ((r = 0; r < ROUNDS; ++r)); do
    for V in $VERTEX_SET; do
        n=${#ARMS[@]}
        for ((j = 0; j < n; ++j)); do
            IFS=: read -r label k <<<"${ARMS[$(((r + j) % n))]}"
            port=$((port + 2)); [ "$port" -gt 47900 ] && port=47300  # dodge TIME_WAIT reuse
            "$BIN/bench_pin_net" sub "$port" "$k" "$PAYLOAD" "$SLOT" "$SLOTS" \
                "$WINDOW_MS" "$V" "$label" &
            subpid=$!
            sleep 0.25
            "$BIN/bench_pin_net" pub "$port" "$PAYLOAD" "$COUNT" "$V" >/dev/null
            wait "$subpid"
        done
    done
done
