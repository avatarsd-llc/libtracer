#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Two-process COMPOSITION-THROUGHPUT comparison (issue #568): how many values a real
# subscriber process observes per second as the composition width K grows, for libtracer
# (one sendmsg(iovec) per K-value group) and for an engine with no composite send (K
# messages for the same K values). Both arms run over real loopback UDP, both count
# deliveries at the RECEIVER, and both are audited for wire use before any number is taken.
#
# Every point is TWO passes, and a row is published only if both hold:
#
#   1. WIRE-USE AUDIT — a short run of the same pub/sub pair with the publisher under
#      `strace -c` (bench/syscall_guard.py). It fails the point if the publisher issued
#      fewer than FLOOR send-family syscalls, which is the check the withdrawn comparison
#      did not have: its Zenoh publisher had no peer, transmitted 5 scouting beacons for
#      520 000 puts, and still reported a rate. The audit is its own pass because tracing
#      costs tens of microseconds per syscall — a traced run's throughput is not a
#      throughput.
#   2. MEASURED PASS — the same pair, untraced. The subscriber emits the RESULT rows from
#      its OWN counts and clock; it exits non-zero and emits nothing at all if it observed
#      no values, any malformed record, or fewer throughput datagrams than the sample floor
#      this script hands it (see run_point).
#
# Each point also carries a NONCE, drawn here and given to both processes, because `magic` is
# a compile-time constant and `width` is the swept parameter: without it a second concurrent
# run of this harness at the same K would have been folded into the rate instead of rejected.
#
# The publisher's send count is captured only to report LOSS. It is never the rate.
#
# NOT DONE HERE, ON PURPOSE: nothing is charted. Issue #568's fifth criterion (re-add the
# comparison chart via render_compare.py) is gated on maintainer sign-off of the published
# number, so this script prints rows and stops. Feeding them to a chart is a separate,
# reviewed step.
#
#   ./run_compose.sh [build-dir]
#
# Env: LIBTRACER_BENCH_COMPOSE_VALUES (values per point, default 400000),
#      LIBTRACER_BENCH_COMPOSE_LAT (paced groups per point, default 4000),
#      COMPOSE_MIN_GROUPS (datagram floor per point, default 20000 — see below),
#      COMPOSE_AUDIT_GROUPS (paced groups in the audit pass, default 400),
#      COMPOSE_SEND_FLOOR (send syscalls the audit demands, default 50),
#      COMPOSE_WIDTHS (default "1 8 64 256"), COMPOSE_VALUE_BYTES (default 64),
#      COMPOSE_WINDOW_MS / COMPOSE_AUDIT_WINDOW_MS (subscriber backstops, default 180000).
set -uo pipefail
cd "$(dirname "$0")"

BUILD="${1:-./build}"
WIDTHS="${COMPOSE_WIDTHS:-1 8 64 256}"
VALUE_BYTES="${COMPOSE_VALUE_BYTES:-64}"
VALUES="${LIBTRACER_BENCH_COMPOSE_VALUES:-400000}"
MIN_GROUPS="${COMPOSE_MIN_GROUPS:-20000}"
AUDIT_GROUPS="${COMPOSE_AUDIT_GROUPS:-400}"
SEND_FLOOR="${COMPOSE_SEND_FLOOR:-50}"
WINDOW_MS="${COMPOSE_WINDOW_MS:-180000}"
# The audit pass is TRACED, so its publisher runs orders of magnitude slower than the
# measured one; its subscriber needs a window sized for that, not for the untraced run.
AUDIT_WINDOW_MS="${COMPOSE_AUDIT_WINDOW_MS:-180000}"
LAT_GROUPS="${LIBTRACER_BENCH_COMPOSE_LAT:-4000}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 || true
cmake --build "$BUILD" --target bench_compose_net -j >/dev/null 2>&1 || true
cmake --build "$BUILD" --target bench_zenoh_compose -j >/dev/null 2>&1 || true

status=0
port=48600
TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT

# run_point <engine> <binary> <K>: audit pass, then measured pass. Prints the subscriber's
# rows on stdout; anything that fails prints COMPOSE_POINT_FAIL and never prints a RESULT.
run_point() {
    local engine="$1" bin="$2" k="$3"
    # THE SAMPLE BUDGET. VALUES is a per-point VALUE budget, so VALUES/k is a DATAGRAM count
    # that falls by K: at the default 400 000 that is 400 000 datagrams at K=1 but 1 562 at
    # K=256 — and K=256 is exactly where "per-value cost stays flat as K grows" gets read off.
    # (A review run of the unfloored version at VALUES=51200 recorded the whole K=256
    # throughput window as 200 datagrams over 0.000880 s.) So the group count is FLOORED — of
    # the four default widths the floor binds at K=64 and K=256 and changes nothing at K=1 or
    # K=8 — and the receiver is handed a floor of its own to enforce on what it actually
    # OBSERVED (min_messages below), so a window that evaporated cannot come back as a rate.
    local groups=$((VALUES / k))
    [ "$groups" -lt "$MIN_GROUPS" ] && groups=$MIN_GROUPS
    # A quarter of the floor, as LOSS HEADROOM: loopback drops under a blast are real and are
    # reported separately (COMPOSE_LOSS), but a point that lost more than three quarters of its
    # datagrams is not a sample worth publishing a rate from. One number, derived — not a second
    # independent knob to keep in step with the first.
    local min_messages=$((MIN_GROUPS / 4))
    # The per-point nonce both processes are given. Without it, `magic` is a compile-time
    # constant and `width` is the swept parameter, so a CONCURRENT run of this same harness at
    # this same K — on this same deterministic port walk — would pass the receiver's checks and
    # be folded into the rate. $RANDOM is 15 bits, so three draws are combined to span the
    # 32-bit field.
    local run_id=$(((RANDOM << 17) ^ (RANDOM << 2) ^ RANDOM))

    # --- pass 1: wire-use audit (traced publisher, no timing taken) ---------------
    # The audit pass asserts deliveries and wire use, not a rate, so its receiver gets no
    # message floor (0): a short traced run is meant to be short.
    port=$((port + 4))
    "$bin" sub "$port" "$k" "$VALUE_BYTES" "$AUDIT_WINDOW_MS" "$run_id" 0 audit \
        >"$TMPD/audit_sub" 2>/dev/null &
    local subpid=$!
    sleep 0.8
    python3 ./syscall_guard.py --floor "$SEND_FLOOR" --label "$engine-K$k" -- \
        "$bin" pub "$port" "$k" "$VALUE_BYTES" 0 "$AUDIT_GROUPS" "$run_id" >"$TMPD/audit_pub" 2>&1
    local guard=$?
    wait "$subpid"
    local audit_sub=$?
    grep -h '^SYSCALL_AUDIT' "$TMPD/audit_pub" 2>/dev/null
    grep -h '^COMPOSE_AUDIT' "$TMPD/audit_sub" 2>/dev/null
    if [ "$guard" -ne 0 ] || [ "$audit_sub" -ne 0 ]; then
        echo -e "COMPOSE_POINT_FAIL\t$engine\t$k\tguard=$guard\taudit_sub=$audit_sub" >&2
        echo -e "COMPOSE_POINT_FAIL\t$engine\t$k\tguard=$guard\taudit_sub=$audit_sub"
        status=1
        return 0
    fi

    # --- pass 2: the measurement (untraced) ---------------------------------------
    port=$((port + 4))
    "$bin" sub "$port" "$k" "$VALUE_BYTES" "$WINDOW_MS" "$run_id" "$min_messages" \
        >"$TMPD/sub" 2>/dev/null &
    subpid=$!
    sleep 0.8
    "$bin" pub "$port" "$k" "$VALUE_BYTES" "$groups" "$LAT_GROUPS" "$run_id" \
        >"$TMPD/pub" 2>/dev/null
    wait "$subpid"
    local measure_sub=$?
    cat "$TMPD/sub"
    local sent observed
    sent=$(awk -F'\t' '/^PUB_SENT/ {print $5}' "$TMPD/pub" 2>/dev/null)
    observed=$(awk -F'\t' '/^RESULT_COMPOSE/ {print $7}' "$TMPD/sub" 2>/dev/null)
    if [ -n "${sent:-}" ] && [ -n "${observed:-}" ]; then
        awk -v e="$engine" -v k="$k" -v s="$sent" -v o="$observed" \
            'BEGIN { loss = 1; if (s > 0) loss = (s - o) / s;
                     printf "COMPOSE_LOSS\t%s\t%s\tsent=%s\tobserved=%s\tloss=%.4f\n",
                            e, k, s, o, loss }'
    fi
    if [ "$measure_sub" -ne 0 ]; then
        echo -e "COMPOSE_POINT_FAIL\t$engine\t$k\tmeasure_sub=$measure_sub" >&2
        status=1
    fi
}

for k in $WIDTHS; do
    if [ -x "$BUILD/bench_compose_net" ]; then
        echo "[libtracer] compose K=$k" >&2
        run_point libtracer "$BUILD/bench_compose_net" "$k"
    else
        echo "libtracer arm missing ($BUILD/bench_compose_net) — nothing to compare" >&2
        status=1
    fi
    if [ -x "$BUILD/bench_zenoh_compose" ]; then
        echo "[zenoh] compose K=$k" >&2
        run_point zenoh "$BUILD/bench_zenoh_compose" "$k"
    else
        echo "(zenoh not vendored — run ./fetch_zenoh.sh for the comparison)" >&2
    fi
done

exit "$status"
