#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The Noise link harness (#2065), best of ROUNDS rounds: the libtracer link arms
# (bench_noise_link), the Zenoh link arms (bench_zenoh_link, when Zenoh is vendored) and the
# crypto backends (bench_noise_crypto). Every arm and every backend runs in its OWN fresh
# process (`--arm=` / `--backend=`, the #1809 rule: no arm inherits another's heap, threads or
# warmed caches), pinned to one CPU set, and every round offers every arm, in the same order,
# so no arm gets a round another was not offered. best_of_rounds.py then keeps each point's
# best per metric (README §best-of-rounds). Every non-RESULT line (CLOCK, NOTE, LINK_RAM,
# NOISE_RAM, FATAL) is kept as printed, round by round, on the same stream.
#
#   bench/run_noise.sh [build-dir] [rounds]      # defaults: bench/build, 3
#   BENCH_CPUS=2-6 bench/run_noise.sh            # the CPU set (default: this shell's own)
#
# Run it on the bench CPUs, under the bench lock, never while perf.yml is in progress (README
# §Noise link harness). A diagnostic, not a gate.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="${1:-$here/build}"
rounds="${2:-3}"
cpus="${BENCH_CPUS:-$(taskset -cp $$ | sed 's/.*: //')}"
raw="$(mktemp -p "${TMPDIR:-$build}")"
trap 'rm -f "$raw"' EXIT
echo "# run_noise: $rounds rounds, one process per arm, pinned to CPUs $cpus" >&2

# One process of $1 with its remaining arguments, pinned.
run_one() {
    local bin="$1"
    shift
    echo "# round $r: $bin $*" >&2
    taskset -c "$cpus" "$build/$bin" "$@" >>"$raw" || echo "# $bin $* exited non-zero in round $r" >&2
}

for ((r = 1; r <= rounds; r++)); do
    for bin in bench_noise_link bench_zenoh_link; do
        if [[ ! -x "$build/$bin" ]]; then
            echo "# $bin not built; skipped" >&2
            continue
        fi
        for arm in $("$build/$bin" --list | grep -E '^[a-z0-9-]+$'); do run_one "$bin" "--arm=$arm"; done
    done
    if [[ -x "$build/bench_noise_crypto" ]]; then
        for b in $("$build/bench_noise_crypto" --list); do run_one bench_noise_crypto "--backend=$b"; done
    else
        echo "# bench_noise_crypto not built; skipped" >&2
    fi
done
python3 "$here/best_of_rounds.py" <"$raw" 2>&1
