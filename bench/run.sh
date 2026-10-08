#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Build and run the libtracer-vs-Zenoh in-process benchmark, printing a
# side-by-side table. Vendor Zenoh first with ./fetch_zenoh.sh (otherwise only
# the libtracer numbers are shown).
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null

echo "================================================================"
echo " libtracer vs Zenoh — IN-PROCESS sweep (fan-out / payload / topics / mixed)"
echo " inproc = write (store+notify+deliver); inproc-borrow = loaned path, no payload"
echo " copy but two allocations per write; inproc-path = write-by-path (registry"
echo " lookup per write; Zenoh's row puts by key, resolved per put); zenoh = peer mode."
echo " Network: ./run_net.sh (UDP, 2 proc).  Response surfaces: ./grid.sh."
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
