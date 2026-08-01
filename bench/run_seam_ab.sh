#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Interleaved A/B/C/... of bench_seam_guard across git revisions (ADR-0072 §4 gate, #576).
#
# Each arm is `name=rev`, where `rev` is any git revision or the literal WORKTREE (the
# current working tree, uncommitted changes included). Every arm is built from the SAME
# harness source and the SAME bench/CMakeLists.txt — the working tree's — so an arm differs
# from its neighbour only in the libtracer it linked. Runs are INTERLEAVED (one rep of every
# arm, then the next rep), which is what makes a thermal/frequency drift hit both arms
# instead of whichever ran second. Report the median across reps.
#
#   usage: bench/run_seam_ab.sh [--reps N] [--cpu C] main=a239b03 branch=HEAD lever1=WORKTREE
#
# Raw SAMPLE lines land in $OUT/samples.txt; the summary table goes to stdout and
# $OUT/summary.txt. OUT defaults to bench/.seam_ab (git-ignored).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPS=11
CPU=2
OUT="${OUT:-$REPO_ROOT/bench/.seam_ab}"
ARMS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --reps) REPS="$2"; shift 2 ;;
        --cpu) CPU="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) ARMS+=("$1"); shift ;;
    esac
done
[[ ${#ARMS[@]} -ge 1 ]] || { echo "usage: $0 [--reps N] [--cpu C] name=rev ..." >&2; exit 2; }

mkdir -p "$OUT"
: > "$OUT/samples.txt"

for arm in "${ARMS[@]}"; do
    name="${arm%%=*}"; rev="${arm#*=}"
    src="$OUT/src-$name"
    rm -rf "$src"; mkdir -p "$src"
    if [[ "$rev" == "WORKTREE" ]]; then
        # Copy the tree (tracked files only) so an uncommitted lever can be measured.
        git -C "$REPO_ROOT" ls-files -z core bench | tar -C "$REPO_ROOT" --null -T - -cf - | tar -C "$src" -xf -
    elif [[ "$rev" == dir:* ]]; then
        # An ablation tree: a copy of some revision with ONE lever backed out, so the arm
        # isolates that lever instead of the whole change.
        cp -r "${rev#dir:}/core" "${rev#dir:}/bench" "$src/"
    else
        git -C "$REPO_ROOT" archive "$rev" core bench | tar -C "$src" -xf -
    fi
    # Pin the harness itself to the WORKING TREE's copy: the arms must differ in the
    # library under test, never in the thing measuring it.
    cp "$REPO_ROOT/bench/bench_seam_guard.cpp" "$src/bench/bench_seam_guard.cpp"
    cp "$REPO_ROOT/bench/CMakeLists.txt" "$src/bench/CMakeLists.txt"
    cp "$REPO_ROOT/bench/bench_common.hpp" "$src/bench/bench_common.hpp"
    echo "== building arm $name ($rev)" >&2
    cmake -S "$src/bench" -B "$OUT/build-$name" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" > "$OUT/cmake-$name.log" 2>&1
    cmake --build "$OUT/build-$name" --target bench_seam_guard -j"$(nproc)" \
        > "$OUT/build-$name.log" 2>&1
done

PIN=(taskset -c "$CPU")
command -v taskset > /dev/null || PIN=()

for ((r = 0; r < REPS; ++r)); do
    for arm in "${ARMS[@]}"; do
        name="${arm%%=*}"
        "${PIN[@]}" "$OUT/build-$name/bench_seam_guard" --arm "$name" --reps 1 >> "$OUT/samples.txt"
    done
    printf '.' >&2
done
echo >&2

python3 - "$OUT/samples.txt" "${ARMS[@]}" <<'PY' | tee "$OUT/summary.txt"
import statistics, sys
path, arms = sys.argv[1], [a.split('=')[0] for a in sys.argv[2:]]
vals = {}
for line in open(path):
    if not line.startswith('SAMPLE'):
        continue
    kv = dict(p.split('=', 1) for p in line.split()[1:])
    vals.setdefault((kv['arm'], kv['leg']), []).append(float(kv['ns_per_op']))
legs = ['read', 'write', 'plain']
base = arms[0]
print(f"{'arm':<24}" + ''.join(f"{l+' ns':>14}" for l in legs) + ''.join(f"{'d '+l:>12}" for l in legs[:2]))
med = {k: statistics.median(v) for k, v in vals.items()}
for a in arms:
    row = f"{a:<24}"
    for l in legs:
        v = med.get((a, l))
        row += f"{v:>14.2f}" if v is not None else f"{'-':>14}"
    for l in legs[:2]:
        v, b = med.get((a, l)), med.get((base, l))
        row += f"{v-b:>+12.2f}" if v is not None and b is not None else f"{'-':>12}"
    print(row)
print()
for a in arms:
    for l in legs:
        v = vals.get((a, l), [])
        if v:
            print(f"spread {a:<20} {l:<6} n={len(v):<3} min={min(v):.2f} med={statistics.median(v):.2f} max={max(v):.2f}")
PY
