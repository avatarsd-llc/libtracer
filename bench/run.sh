#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
#
# Build and run the libtracer-vs-Zenoh in-process benchmark, printing a
# side-by-side table. Vendor Zenoh first with ./fetch_zenoh.sh (otherwise only
# the libtracer numbers are shown).
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null

echo "================================================================"
echo " libtracer vs Zenoh — IN-PROCESS pub/sub (1 publisher, 1 subscriber)"
echo " Not a network benchmark: libtracer has no socket transport yet (M5)."
echo " libtracer/inproc = zero-copy graph dispatch; libtracer/loopback ="
echo " encode+ROUTER+decode over an in-memory queue; zenoh/inproc = peer mode."
echo "================================================================"

res="$(mktemp)"
./build/bench_libtracer >>"$res"
if [ -x ./build/bench_zenoh ]; then
    ./build/bench_zenoh >>"$res"
else
    echo "(zenoh not vendored — run ./fetch_zenoh.sh for the comparison)"
fi
python3 collate.py <"$res"
rm -f "$res"
