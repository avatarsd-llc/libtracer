#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Run a publisher under ``strace -c`` and FAIL if it never reached the wire.

Issue #568, acceptance criterion 3. The comparison this guard protects was once published
with one engine doing **no I/O at all**: its publisher declared a topic with no subscriber
and no peer, so ``put()`` returned without transmitting, and the run still reported a rate.
Measured after the fact, that engine had issued **5 ``sendto`` and 10 ``write`` for 520 000
puts** — and the five were multicast scouting beacons. Nothing in the harness looked, so
nothing objected.

So this is the check that makes a number *refusable*: a comparison harness must not be able
to report a figure for an engine that never transmitted.

What is counted
---------------
Only the datagram-send family — ``sendmsg``, ``sendto``, ``sendmmsg``. ``write``/``writev``
are traced and reported, but deliberately do **not** count toward the floor: in a ``-c``
summary a socket write and a ``printf`` to stdout are the same row, so a floor that accepted
``write`` would pass on an engine that only ever wrote its own output. The composition bench
is UDP on both arms, where the send family is exactly the wire.

Why the audit is its OWN pass
-----------------------------
``strace`` ptrace-stops every traced syscall, which is worth tens of microseconds each; a
traced run's throughput is not the engine's throughput. The audit is therefore a short,
separate run of the same publisher against the same live subscriber, and the measured run is
untraced. The two guards are complementary and neither is optional: the audit proves the
wire was used, and the subscriber's own delivery count (bench_compose.hpp) proves the bytes
arrived — a number is published only when both hold.

Failing loudly beats skipping
-----------------------------
No ``strace``, no permission to ptrace, a summary that cannot be parsed: every one of those
exits non-zero. A guard that quietly downgrades to "not checked" is the defect it was
written to prevent, wearing a passing badge.

    syscall_guard.py --floor 50 --label zenoh-pub -- \
        ./build/bench_zenoh_compose pub 48610 8 64 0 400 12345
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

# The send-family syscalls that constitute "it reached the wire" for a datagram transport.
SEND_SYSCALLS = ("sendmsg", "sendto", "sendmmsg")
# Traced for the record, never counted — see the module docstring.
REPORT_SYSCALLS = ("write", "writev")

# Beyond-scouting floor, and why it is a small CONSTANT rather than a fraction of the messages
# the publisher was asked to send. Both ends of the range were measured on this harness:
#
#   * A peerless publisher — the recorded defect, reproduced — issued **3 sendto and 8 write
#     for 4 000 puts**, matching the 5 sendto / 10 write recorded for 520 000 puts.
#   * A publisher with a live subscriber BATCHES: 3 200 puts left as **479** sendto. So a floor
#     expressed as "at least one send per message" would fail a perfectly honest run, and the
#     guard would be teaching the harness to distrust batching, which is the very thing the
#     comparison is about.
#
# 50 sits an order of magnitude above the peerless case and an order of magnitude below the
# batched one, and the actual count is always printed beside it so the margin is visible rather
# than assumed. Raise it with --floor when the audit pass is long enough to justify it.
DEFAULT_FLOOR = 50


def parse_strace_counts(text: str) -> dict[str, int]:
    """@brief Syscall -> call count, from a ``strace -c`` summary.

    The summary's columns are ``% time / seconds / usecs-per-call / calls / errors / syscall``
    with ``errors`` omitted when there were none, so the row is read as "calls is field 3,
    the syscall name is the last field" — true under both widths. Header, rule and ``total``
    rows are skipped. A run whose traced syscalls never fired produces an EMPTY summary, which
    correctly yields ``{}`` and therefore zero sends.
    """
    counts: dict[str, int] = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 5 or fields[0].startswith("-") or fields[-1] in ("syscall", "total"):
            continue
        try:
            counts[fields[-1]] = counts.get(fields[-1], 0) + int(fields[3])
        except ValueError:
            continue
    return counts


def sends(counts: dict[str, int]) -> int:
    """@brief Total datagram-send syscalls in a parsed summary."""
    return sum(counts.get(name, 0) for name in SEND_SYSCALLS)


def audit(cmd: list[str], floor: int, label: str) -> int:
    """@brief Run @p cmd under strace and report its wire use.

    @return 0 when the send count cleared @p floor; 1 when it did not; 2 when the audit could
            not be performed at all (no strace, no ptrace permission, unparseable summary).
    """
    strace = shutil.which("strace")
    if strace is None:
        print(f"SYSCALL_AUDIT\t{label}\tverdict=UNAVAILABLE\treason=no-strace", flush=True)
        return 2
    traced = ",".join(SEND_SYSCALLS + REPORT_SYSCALLS)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "counts.txt")
        proc = subprocess.run([strace, "-f", "-c", "-e", f"trace={traced}", "-o", out, "--"] + cmd,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False)
        try:
            with open(out, encoding="utf-8", errors="replace") as fh:
                summary = fh.read()
        except OSError:
            summary = ""
            proc = subprocess.CompletedProcess(cmd, 127, stderr=b"no strace summary written")
        if proc.returncode != 0 and not summary.strip():
            why = proc.stderr.decode("utf-8", "replace").strip().replace("\t", " ")[:200]
            print(f"SYSCALL_AUDIT\t{label}\tverdict=UNAVAILABLE\treason={why or 'strace-failed'}",
                  flush=True)
            return 2
    counts = parse_strace_counts(summary)
    n = sends(counts)
    verdict = "PASS" if n >= floor else "FAIL"
    detail = " ".join(f"{k}={counts.get(k, 0)}" for k in SEND_SYSCALLS + REPORT_SYSCALLS)
    print(f"SYSCALL_AUDIT\t{label}\tverdict={verdict}\tsends={n}\tfloor={floor}\t{detail}",
          flush=True)
    return 0 if verdict == "PASS" else 1


def main(argv: list[str] | None = None) -> int:
    """@brief CLI entry point."""
    ap = argparse.ArgumentParser(description="fail a bench run whose publisher never transmitted")
    ap.add_argument("--floor", type=int, default=DEFAULT_FLOOR,
                    help=f"minimum send-family syscalls (default {DEFAULT_FLOOR})")
    ap.add_argument("--label", default="pub", help="tag for the SYSCALL_AUDIT line")
    ap.add_argument("cmd", nargs=argparse.REMAINDER, help="-- <command to trace>")
    args = ap.parse_args(argv)
    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd:
        ap.error("no command given (use: syscall_guard.py --floor N -- <cmd...>)")
    return audit(cmd, args.floor, args.label)


if __name__ == "__main__":
    sys.exit(main())
