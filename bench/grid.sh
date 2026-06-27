#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# System-dynamics grid: sweep the log-spaced (payload x fan-out) and
# (payload x topics) response surfaces for libtracer and zenoh, then render 2D
# line plots, 3D surfaces, and libtracer/zenoh speedup heatmaps into figures/.
# Vendor zenoh first with ./fetch_zenoh.sh; plotting needs the venv (see below).
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null

echo "system,size,fanout,endpoints,pub_s,deliv_s,p50_ns,p99_ns,mean_ns" >grid.csv
./build/bench_libtracer grid | grep -v '^system,' >>grid.csv
if [ -x ./build/bench_zenoh ]; then
    ./build/bench_zenoh grid | grep -v '^system,' >>grid.csv
else
    echo "(zenoh not vendored — run ./fetch_zenoh.sh for the comparison)" >&2
fi
echo "wrote grid.csv ($(( $(wc -l <grid.csv) - 1 )) rows)"

# Plotting venv:  python3 -m venv .venv && ./.venv/bin/pip install matplotlib numpy
if [ -x ./.venv/bin/python ]; then
    ./.venv/bin/python plot.py grid.csv figures
else
    echo "(no .venv — create it: python3 -m venv .venv && ./.venv/bin/pip install matplotlib numpy)" >&2
fi
