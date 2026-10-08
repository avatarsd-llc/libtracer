#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Bank the perf gate's A/A null on the bench host (#1909): the procedure, end to end.
#
# The null must be measured on the gate's own host and pin (bench.slice, single-threaded
# families on CPU 3, multi-threaded ones on 3-6) with the bench runner STOPPED, so nothing
# else lands on those CPUs. A fit is MIN_ROUNDS (25) rounds or more, and one round of three
# builds takes about 2.5 minutes, so the fit is measured in several WINDOWS, each one runner
# stop of well under 45 minutes, plus one more window the fit never sees (held out):
#
#   1. build one source three times, at three function alignments (code layout moves the
#      way an unrelated change moves it), off the bench CPUs and at low priority;
#   2. for each window: wait until no perf.yml or perf-local.yml run is queued or in
#      progress, stop the bench runner, `aa_null.py measure` in bench.slice, and start the
#      runner again. The runner is restarted after EVERY window and on ANY exit of this
#      script, failures and Ctrl-C included (the EXIT trap);
#   3. `aa_null.py bank` over the pooled fit windows, with the held-out window replayed
#      into the null's meta, written to bench/aa_null.json of this checkout.
#
# Run it from a checkout of the commit to bank (normally main, after the bench changes it
# should reflect have merged), on the bench host, as a user with passwordless sudo:
#
#   bench/aa_null_campaign.sh                       # 3 fit windows x 9 rounds + 10 held out
#   WINDOWS=2 ROUNDS=13 bench/aa_null_campaign.sh   # 26 rounds in two longer stops
#   BENCH_LOCK=~/scratch/bench.lock bench/aa_null_campaign.sh   # share the CPUs by a lock
#
# Then read the printed held-out line (A/A false-fail sessions, rows an injected 10% fails
# in every session), update the capped-row list in docs/methodology.md from
# `aa_null.py bank`'s printout, and commit bench/aa_null.json through a pull request.
set -euo pipefail

REPO="${REPO:-avatarsd-llc/libtracer}"
UNIT="${UNIT:-actions.runner.avatarsd-llc-libtracer.studio-bench.service}"
SLICE="${SLICE:-bench.slice}"
BENCH_CPU="${BENCH_CPU:-3-6}"
BENCH_CPU_SINGLE="${BENCH_CPU_SINGLE:-3}"
WINDOWS="${WINDOWS:-3}"          # fit windows (runner stops)
ROUNDS="${ROUNDS:-9}"            # rounds per fit window: WINDOWS x ROUNDS >= 25
HELD_OUT_ROUNDS="${HELD_OUT_ROUNDS:-10}"
POLL_S="${POLL_S:-60}"
# Optional: a lock file other bench users on this host take too (`flock <file> <cmd>`). Each
# window holds it from before the runner stops until the runner is started again, so a
# window never shares the bench CPUs with another bench, and one window is one lock stretch.
BENCH_LOCK="${BENCH_LOCK:-}"
JOBS="${JOBS:-8}"
ROOT="$(git rev-parse --show-toplevel)"
REV="$(git -C "$ROOT" rev-parse --short HEAD)"
WORK="${WORK:-/home/$USER/scratch/aa-null-$REV}"
TARGETS=(bench_libtracer bench_forward_heap bench_compact_delivery bench_forward_demux
         bench_store_sweep)

if (( WINDOWS * ROUNDS < 25 )); then
  echo "aa_null_campaign: WINDOWS x ROUNDS = $((WINDOWS * ROUNDS)) is under the 25-round fit" >&2
  exit 2
fi
sudo -n true  # fail now, not after the builds, when sudo would prompt

runner_stopped=0
restart_runner() {
  if (( runner_stopped )); then
    sudo -n systemctl start "$UNIT" && runner_stopped=0 && echo "aa_null_campaign: $UNIT started"
  fi
}
trap restart_runner EXIT INT TERM

# --- 1. three layouts of one source, built off the bench CPUs ---------------------------
mkdir -p "$WORK"
if [[ ! -d "$WORK/src" ]]; then
  mkdir "$WORK/src"
  git -C "$ROOT" archive HEAD | tar -x -C "$WORK/src"
fi
builds=()
for align in default 32 64; do
  dir="$WORK/$REV-align-$align"
  flags=()
  [[ $align != default ]] && flags=(-DCMAKE_CXX_FLAGS="-falign-functions=$align")
  systemd-run --user --scope -q -p CPUWeight=20 nice -n 19 \
    cmake -S "$WORK/src/bench" -B "$dir" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "${flags[@]}" >/dev/null
  systemd-run --user --scope -q -p CPUWeight=20 nice -n 19 \
    cmake --build "$dir" --target "${TARGETS[@]}" --parallel "$JOBS" >/dev/null
  builds+=(--build "$dir")
done

# --- 2. one runner stop per window --------------------------------------------------------
perf_busy() {
  local wf st n
  for wf in perf.yml perf-local.yml; do
    for st in queued in_progress; do
      n="$(gh run list -R "$REPO" -w "$wf" -s "$st" --json databaseId -q length)"
      (( n > 0 )) && { echo "$wf has $n run(s) $st"; return 0; }
    done
  done
  return 1
}

measure_window() {  # $1 = output file, $2 = rounds
  local why lock_fd=
  while :; do
    while why="$(perf_busy)"; do
      echo "aa_null_campaign: waiting, $why"
      sleep "$POLL_S"
    done
    [[ -z $BENCH_LOCK ]] && break
    exec {lock_fd}>>"$BENCH_LOCK"
    echo "aa_null_campaign: waiting for $BENCH_LOCK"
    flock "$lock_fd"
    # A perf run may have been queued while another bench held the lock: look again.
    why="$(perf_busy)" || break
    exec {lock_fd}>&-
    lock_fd=
  done
  sudo -n systemctl stop "$UNIT"
  runner_stopped=1
  echo "aa_null_campaign: $UNIT stopped; measuring $2 rounds -> $1"
  local rc=0
  sudo -n systemd-run --scope -q --slice="$SLICE" --uid="$(id -u)" --gid="$(id -g)" \
    env HOME="$HOME" PATH="$PATH" BENCH_CPU="$BENCH_CPU" BENCH_CPU_SINGLE="$BENCH_CPU_SINGLE" \
    python3 "$ROOT/bench/aa_null.py" measure "${builds[@]}" --rounds "$2" \
    --host "bench host, $SLICE CPUs $BENCH_CPU (single-threaded on $BENCH_CPU_SINGLE), runner stopped" \
    --out "$1" || rc=$?
  restart_runner
  if [[ -n $lock_fd ]]; then
    exec {lock_fd}>&-
    sleep "$POLL_S"  # a bench waiting on the lock takes it before the next window does
  fi
  return "$rc"
}

fits=()
for ((w = 1; w <= WINDOWS; w++)); do
  out="$WORK/fit-$w.json"
  [[ -s $out ]] || measure_window "$out" "$ROUNDS"
  fits+=(--raw "$out")
done
held="$WORK/held-out.json"
[[ -s $held ]] || measure_window "$held" "$HELD_OUT_ROUNDS"

# --- 3. bank the pooled fit, with the held-out replay in its meta --------------------------
python3 "$ROOT/bench/aa_null.py" bank "${fits[@]}" --held-out "$held" \
  --out "$ROOT/bench/aa_null.json"
python3 "$ROOT/bench/aa_null.py" evaluate --raw "$held" --null "$ROOT/bench/aa_null.json"
