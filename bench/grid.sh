#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Local preview of the libtracer-vs-Zenoh comparison: sweep the (payload x fan-out)
# and (payload x topics) response surfaces for both engines, then render the SAME
# absolute-value charts the docs publish into a self-contained preview.html you can
# open in a browser. Vendor Zenoh first with ./fetch_zenoh.sh (otherwise only the
# libtracer numbers appear). CI does this during the docs build (docs.yml); this is
# the offline equivalent — no matplotlib, no committed figures.
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --target bench_libtracer bench_transports bench_scatter -j >/dev/null
cmake --build build --target bench_zenoh bench_zenoh_net -j >/dev/null 2>&1 || true

res="$(mktemp)"
./build/bench_libtracer grid >>"$res" 2>/dev/null                # in-process axes
./build/bench_scatter >>"$res" 2>/dev/null                       # network throughput vs composition K
if [ -x ./build/bench_zenoh ]; then
    ./build/bench_zenoh grid >>"$res" 2>/dev/null
    ./build/bench_zenoh scatter >>"$res" 2>/dev/null             # zenoh's flat timer-batched put rate
else
    echo "(zenoh not vendored — run ./fetch_zenoh.sh for the comparison)" >&2
fi
bash run_net.sh >>"$res" 2>/dev/null || true                     # network per-transport latency (UDP/TCP, 2-process)

python3 render_compare.py --standalone --prov "local preview ($(date -u +%Y-%m-%dT%H:%MZ))" \
    <"$res" >preview.html
rm -f "$res"
echo "wrote preview.html — open it in a browser for the absolute-value charts."
