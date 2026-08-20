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

# THE A/A NULL IS AN ARM, NOT A POST-HOC ESTIMATE. `B2-null` is a byte-for-byte duplicate of
# `B-sentinel` — same K = 0, same binary, same pool, same transport — rotated through the same
# interleave. The B-sentinel-vs-B2-null spread at a given (vertices, payload, slot) cell IS the
# instrument's null, and no pinning-arm delta narrower than it may be quoted. It has to be an
# in-band arm here because collate_pin.py's paired sign test covers only the store-leg table:
# RESULT_PINNET carries no round index, so the net table is unpaired min/med/max and per-round
# pairing is unavailable on this leg.
#
# D16 exists because the predicate is `payload * K >= segment_bytes`. At slot = 1024 a 512 B
# payload pins from K >= 2, but a 64 B payload needs K >= 16 — without D16 the 64 B sweep would
# have NO arm that pins below C-pin-always and its D-rows would be vacuous.
#
# arm : K
ARMS=(
    "B-sentinel:0"
    "B2-null:0"
    "D2:2"
    "D4:4"
    "D8:8"
    "D16:16"
    "C-pin-always:4294967295"
)

VERTEX_SET="${VERTEX_SET:-1 8 32 128}"

port=47300

# One (subscriber, publisher) pair, RETRIED on a failed bind.
#
# A full sweep is rounds x |VERTEX_SET| x |ARMS| pairs and burns two UDP ports each, so it wraps
# the port window several times and eventually lands on one the kernel has not released. That
# used to abort the whole sweep under `set -e` mid-round, silently truncating the transcript to
# a partial rotation — the arms are interleaved, so a truncated run is also an UNBALANCED one.
# Retrying on the next port keeps the rotation balanced; five consecutive failures is a host
# problem and still stops the run loudly.
#
# Both ends are checked. A subscriber that failed to bind is dead within the settle sleep, and
# a publisher that failed to bind would otherwise leave the subscriber to run its whole window
# and emit a zero-delivery row that looks like starvation but is a bind error.
run_cell() {
    local label="$1" k="$2" V="$3" subpid
    for _ in 1 2 3 4 5; do
        port=$((port + 2)); [ "$port" -gt 48900 ] && port=47300
        "$BIN/bench_pin_net" sub "$port" "$k" "$PAYLOAD" "$SLOT" "$SLOTS" \
            "$WINDOW_MS" "$V" "$label" &
        subpid=$!
        sleep 0.25
        if ! kill -0 "$subpid" 2>/dev/null; then
            wait "$subpid" || true          # subscriber bind failed; next port
            continue
        fi
        if ! "$BIN/bench_pin_net" pub "$port" "$PAYLOAD" "$COUNT" "$V" >/dev/null; then
            kill "$subpid" 2>/dev/null || true
            wait "$subpid" || true          # publisher bind failed; discard the pair
            continue
        fi
        wait "$subpid" && return 0
    done
    echo "run_pin_net: $label V=$V could not bind after 5 attempts" >&2
    return 1
}

for ((r = 0; r < ROUNDS; ++r)); do
    for V in $VERTEX_SET; do
        n=${#ARMS[@]}
        for ((j = 0; j < n; ++j)); do
            IFS=: read -r label k <<<"${ARMS[$(((r + j) % n))]}"
            run_cell "$label" "$k" "$V"
        done
    done
done
