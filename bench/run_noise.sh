#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The Noise link harness (#2065), best of ROUNDS rounds: the libtracer link arms
# (bench_noise_link), the Zenoh link arms (bench_zenoh_link, when Zenoh is vendored) and the
# crypto arm (bench_noise_crypto), each binary once per round, in the same loop, so no arm gets
# a round another arm was not offered. best_of_rounds.py then keeps each point's best per metric
# (README §best-of-rounds). Every non-RESULT line (CLOCK, NOTE, LINK_RAM, NOISE_RAM, FATAL) is
# kept as printed, round by round, on the same stream.
#
#   bench/run_noise.sh [build-dir] [rounds]      # defaults: bench/build, 3
#
# Run it on the bench CPUs, under the bench lock, never while perf.yml is in progress (README
# §Noise link harness). A diagnostic, not a gate.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="${1:-$here/build}"
rounds="${2:-3}"
raw="$(mktemp)"
trap 'rm -f "$raw"' EXIT

for ((r = 1; r <= rounds; r++)); do
    for bin in bench_noise_link bench_zenoh_link bench_noise_crypto; do
        if [[ -x "$build/$bin" ]]; then
            echo "# round $r: $bin" >&2
            "$build/$bin" >>"$raw" || echo "# $bin exited non-zero in round $r" >&2
        else
            echo "# $bin not built; skipped" >&2
        fi
    done
done
python3 "$here/best_of_rounds.py" <"$raw" 2>&1
