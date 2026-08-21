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
cmake --build build --target bench_libtracer bench_transports -j >/dev/null
cmake --build build --target bench_zenoh bench_zenoh_net -j >/dev/null 2>&1 || true

# ROUNDS executions of BOTH engines, reduced to the best observation per point by
# best_of_rounds.py. Contamination on a shared host is one-sided — a neighbour can only
# make a round slower — so the best round is the estimator closest to the code, and a
# median would report how busy the machine was. Both engines are run the same number of
# times inside the same loop so neither arm can bank a luckier round than the other.
ROUNDS="${ROUNDS:-3}"
raw="$(mktemp)"
for r in $(seq 1 "$ROUNDS"); do
    echo "round $r/$ROUNDS…" >&2
    ./build/bench_libtracer grid >>"$raw" 2>/dev/null             # in-process axes
    if [ -x ./build/bench_zenoh ]; then
        ./build/bench_zenoh grid >>"$raw" 2>/dev/null
    elif [ "$r" = 1 ]; then
        echo "(zenoh not vendored — run ./fetch_zenoh.sh for the comparison)" >&2
    fi
done

res="$(mktemp)"
python3 best_of_rounds.py <"$raw" >"$res"
rm -f "$raw"
bash run_net.sh >>"$res" 2>/dev/null || true                     # network per-transport latency (UDP/TCP, 2-process)

python3 render_compare.py --standalone --prov "local preview ($(date -u +%Y-%m-%dT%H:%MZ), best of $ROUNDS rounds)" \
    <"$res" >preview.html
rm -f "$res"
echo "wrote preview.html — open it in a browser for the absolute-value charts."
