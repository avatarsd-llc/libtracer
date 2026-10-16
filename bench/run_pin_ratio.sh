#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# RFC-0022 §6, host half — the interleaved round runner.
#
# One ROUND runs every arm exactly once, all of them inside ONE process, rotating their own
# order per round (`--round0`). Exhausting one arm's runs before starting the next is the shape
# that produced the recorded 55.2 / 53.0 / 149.8 M deliv/s swing on identical code, and nothing
# here does that.
#
# THE CONTROL ARM IS B-copy (the `COPY` hint), in this same binary: the one-copy store branch,
# on the same binary as every sharing arm. The other arms are the RFC-0022 Amendment 3 hints:
# D-default (the build's block-source trait) and C-share (`SHARE`). collate_pin.py takes every
# other arm's verdict as a paired per-round delta against B-copy.
#
#   run_pin_ratio.sh <bench-dir> [rounds] > raw.tsv
#
# The directory is a built bench/build. Every raw line is emitted verbatim on stdout —
# collate_pin.py reads it.
set -euo pipefail

BIN="${1:?bench build dir}"
ROUNDS="${2:-10}"

ARMS="B-copy,D-default,C-share"

# Reachability before numbers, every run.
"$BIN/bench_pin_ratio" --calibrate || { echo "calibration FAILED" >&2; exit 2; }

for ((r = 0; r < ROUNDS; ++r)); do
    "$BIN/bench_pin_ratio" --rounds=1 --round0="$r" --arms="$ARMS"
done

# Repeat the line-break AFTER the run: an instrument that was live at the start and inert by
# the end would otherwise sign off on every cell in between.
"$BIN/bench_pin_ratio" --calibrate || { echo "POST calibration FAILED" >&2; exit 2; }
