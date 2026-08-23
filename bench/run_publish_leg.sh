#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The #1485 / #1495 re-banking runner: the two instruments RFC-0025 §4.6.2's 2026-08-24
# erratum says are owed for its demoted absolutes, run under the discipline the erratum
# faults their predecessor for lacking — a quiet host, best-of-rounds, and a stamped
# provenance descriptor on every emitted point.
#
#   run_publish_leg.sh <bench-build-dir> [rounds] [cpu] [out-dir]
#
# Rounds default to 5, the pinned CPU to 4, and the output directory to a fresh
# `publish-leg-<date>` beside this script. Writes there:
#
#   round-N-<bench>.txt   every round's raw transcript, verbatim
#   best.txt              the best-of-rounds reduction of all of them
#   points-{ns,tput}.json github-action-benchmark points, host-stamped
#
# BEST-OF-ROUNDS, NEVER MEDIAN (docs/methodology.md, and `best_of_rounds.py`'s own header):
# contamination on a shared host is one-sided, so the median of N rounds estimates how busy
# the box was and the best estimates what the code can do.
#
# PINNING. `bench_publish_leg` is single-threaded and is pinned; `bench_writer_fanin` is a
# thread sweep and is NOT — a many-writer arm on one logical CPU measures the scheduler.
# The two therefore carry different provenance notes, and neither is comparable with the
# other's window.
#
# QUIET HOST. `host_guard.py wait` holds until the load average is under the bar before the
# first round. It does not detect an orphaned busy-loop that is *already* counted in the
# load average as normal work, so the standing pre-bench check still applies: look for stray
# `while :` loops from old perf forks (`ps -eo pid,etime,pcpu,args --sort=-pcpu | head`) and
# make sure no CI run is in flight on this host's self-hosted runners.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${1:?bench build dir (e.g. bench/build)}"
ROUNDS="${2:-5}"
CPU="${3:-4}"
OUT="${4:-$HERE/publish-leg-$(date -u +%Y%m%d)}"

for b in bench_publish_leg bench_writer_fanin; do
    [ -x "$BIN/$b" ] || { echo "missing $BIN/$b — build it first" >&2; exit 2; }
done

mkdir -p "$OUT"
python3 "$HERE/host_guard.py" wait --timeout 900

for ((r = 1; r <= ROUNDS; ++r)); do
    taskset -c "$CPU" "$BIN/bench_publish_leg" >"$OUT/round-$r-publish_leg.txt"
    "$BIN/bench_writer_fanin" >"$OUT/round-$r-writer_fanin.txt"
done

cat "$OUT"/round-*.txt | python3 "$HERE/best_of_rounds.py" >"$OUT/best.txt"

python3 "$HERE/perf_emit_benchmark.py" \
    $(for f in "$OUT"/round-*.txt; do printf ' --raw %s' "$f"; done) \
    --out-smaller "$OUT/points-ns.json" --out-bigger "$OUT/points-tput.json"

DESC="${HOST_DESC:-$(uname -sm), $(nproc) cpus, $(hostname)}"
python3 "$HERE/host_guard.py" stamp \
    --json "$OUT/points-ns.json" --json "$OUT/points-tput.json" \
    --desc "$DESC; best of $ROUNDS rounds; publish_leg pinned to cpu $CPU, writer_fanin unpinned" \
    --compiler --note "${HOST_NOTE:-}"

echo "-- best of $ROUNDS rounds --"
grep -E '^RESULT\s' "$OUT/best.txt" || true
echo "raw + stamped points in $OUT"
