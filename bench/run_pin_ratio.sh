#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# RFC-0022 §6, host half — the interleaved round runner.
#
# One ROUND runs every arm exactly once. Arms B/C/D live in one process and rotate their own
# order per round (`--round0`); arm A is necessarily a second binary (it IS untouched
# origin/main), so its position relative to that process alternates round by round. Exhausting
# one arm's runs before starting the next is the shape that produced the recorded 55.2 / 53.0 /
# 149.8 M deliv/s swing on identical code, and nothing here does that.
#
#   run_pin_ratio.sh <impl-bench-dir> <control-bench-dir> [rounds] > raw.tsv
#
# Both directories are a built bench/build; the control one must be a worktree of untouched
# origin/main. Every raw line is emitted verbatim on stdout — collate_pin.py reads it.
set -euo pipefail

IMPL="${1:?impl bench build dir}"
CTL="${2:?control bench build dir}"
ROUNDS="${3:-10}"

IMPL_ARMS="B-sentinel,D2,D4,D8,D64,D1024,C-pin-always"

# Reachability before numbers, both binaries, every run.
"$CTL/bench_pin_ratio" --calibrate  || { echo "control calibration FAILED" >&2; exit 2; }
"$IMPL/bench_pin_ratio" --calibrate || { echo "impl calibration FAILED"    >&2; exit 2; }

for ((r = 0; r < ROUNDS; ++r)); do
    if (( r % 2 == 0 )); then
        "$CTL/bench_pin_ratio"  --rounds=1 --round0="$r" --arms=A-control
        "$IMPL/bench_pin_ratio" --rounds=1 --round0="$r" --arms="$IMPL_ARMS"
    else
        "$IMPL/bench_pin_ratio" --rounds=1 --round0="$r" --arms="$IMPL_ARMS"
        "$CTL/bench_pin_ratio"  --rounds=1 --round0="$r" --arms=A-control
    fi
done

# Repeat the line-break AFTER the run: an instrument that was live at the start and inert by
# the end would otherwise sign off on every cell in between.
"$CTL/bench_pin_ratio" --calibrate  || { echo "control POST calibration FAILED" >&2; exit 2; }
"$IMPL/bench_pin_ratio" --calibrate || { echo "impl POST calibration FAILED"    >&2; exit 2; }
