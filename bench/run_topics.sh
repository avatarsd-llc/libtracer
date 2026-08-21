#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# #1485 addendum C — the TOPIC-COUNT comparison arm, run under the fairness rules
# docs/methodology.md sets out.
#
# What it fixes. The published topic-count comparison put libtracer's `inproc-path` row (write BY
# ADDRESS — a registry resolution inside every timed iteration) against bench_zenoh's row of the
# same name, which publishes through a DECLARED Publisher and resolves nothing per put. So one
# arm carried a resolution term the other did not, and the reported narrowing of the margin
# across the ladder could not be attributed to either engine's topic scaling. Both engines now
# emit BOTH spellings — `topics-bound` (pre-bound handle / declared publisher) and `topics-addr`
# (destination resolved inside the operation) — over the same kTopicLadder.
#
# The rules this obeys, all from docs/methodology.md's Fairness section:
#   * deliveries are COUNTED AT THE SUBSCRIBER on both arms, never publishes x fan-out — that is
#     already how both binaries report, and nothing here recomputes it;
#   * best-of-ROUNDS, never median: contamination on this host is one-sided, and a
#     median-of-rounds reduction once turned a +/-0.34% instrument into one reading -33%..+54%;
#   * BOTH engines run the same number of rounds inside the same loop, so neither picks up a
#     retry the other was not offered;
#   * BOTH ARM ORDERS are run (`topics` then `topics-rev`) and both engine orders are alternated
#     per round. An always-same-first ordering manufactured an apparent 12.55-vs-8.07 M/s win on
#     this box that vanished on the flip; a point whose two orders disagree on the sign of a
#     trend is unresolved, and no verdict may be drawn from it.
#
# It also VERIFIES the zenoh arm was built rather than trusting a green run: bench/CMakeLists.txt
# discovers the vendored zenoh-cpp directory, and when that discovery fails the zenoh targets are
# skipped with a STATUS message and the comparison would quietly publish one engine's numbers
# with no opponent beside them.
#
# usage: ./run_topics.sh [rounds]     (default 5)
set -euo pipefail
cd "$(dirname "$0")"

ROUNDS="${1:-5}"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null

if [ ! -x ./build/bench_zenoh ]; then
    echo "error: ./build/bench_zenoh was NOT built — the zenoh arm is absent." >&2
    echo "  Run ./fetch_zenoh.sh (it owns ZVER) and re-configure. A missing opponent must be" >&2
    echo "  loud: a one-engine 'comparison' is what this check exists to prevent." >&2
    exit 1
fi

echo "HOST loadavg=$(cat /proc/loadavg) cc1plus=$(pgrep -c cc1plus || echo 0) nproc=$(nproc)"
echo "ZENOH vendor=$(ls -d vendor/zenoh-cpp-* 2>/dev/null | tr '\n' ' ')"

raw="$(mktemp)"
trap 'rm -f "$raw"' EXIT

for r in $(seq 1 "$ROUNDS"); do
    if [ $((r % 2)) -eq 1 ]; then
        ./build/bench_libtracer topics >>"$raw"
        ./build/bench_zenoh topics >>"$raw"
    else
        ./build/bench_zenoh topics-rev >>"$raw"
        ./build/bench_libtracer topics-rev >>"$raw"
    fi
done

python3 best_of_rounds.py <"$raw"
echo "HOST-AFTER loadavg=$(cat /proc/loadavg) cc1plus=$(pgrep -c cc1plus || echo 0)"
