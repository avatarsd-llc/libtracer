#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Two-process NETWORK benchmark: libtracer-UDP vs Zenoh-UDP over localhost. Each
# system runs a separate publisher and subscriber process that cross the kernel
# UDP stack. Vendor Zenoh first with ./fetch_zenoh.sh for the comparison.
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null

echo "================================================================"
echo " libtracer vs Zenoh — NETWORK pub/sub over localhost UDP (2 processes)"
echo " Real kernel network path. One-way latency via CLOCK_MONOTONIC (shared"
echo " across processes on one host). UDP is best-effort: drops shrink the count."
echo "================================================================"

res="$(mktemp)"

echo "[libtracer] sub:48000  pub:48001 -> 48000 …" >&2
./build/bench_libtracer_net sub 48000 48001 >>"$res" &
sub=$!
sleep 0.4
./build/bench_libtracer_net pub 48001 48000 >/dev/null
wait "$sub"

if [ -x ./build/bench_zenoh_net ]; then
    echo "[zenoh] sub listen udp:48010, pub connect -> 48010 …" >&2
    ./build/bench_zenoh_net sub 48010 >>"$res" &
    sub=$!
    sleep 0.6
    ./build/bench_zenoh_net pub 48010 >/dev/null
    wait "$sub"
else
    echo "(zenoh not vendored — run ./fetch_zenoh.sh for the comparison)" >&2
fi

python3 collate.py <"$res"
rm -f "$res"
