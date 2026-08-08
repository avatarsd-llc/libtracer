#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Tests for the composition bench's wire-use guard (#568 criterion 3).

A guard is only worth its line count if it has been seen to FAIL. The one this file tests
exists because a published comparison once reported a rate for an engine that never
transmitted, so the tests here are written in both directions: a process that really sends
must PASS, and a process that sends nothing must FAIL. The negative cases are the point —
``test_true_transmits_nothing`` is the exact shape of the original defect, run against the
real ``strace``.

    python3 bench/test_compose_guard.py      # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import syscall_guard  # noqa: E402

# A real `strace -c` summary with the errors column present on one row and absent on the
# other — the two widths the parser has to read the same way.
SUMMARY_MIXED = """% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 60.00    0.001000           2       500           sendmsg
 40.00    0.000500           1       300        12 sendto
------ ----------- ----------- --------- --------- ----------------
100.00    0.001500                   800        12 total
"""

# What a run that never reached the wire leaves behind: strace writes an EMPTY summary when
# none of the traced syscalls ever fired.
SUMMARY_EMPTY = ""

# The recorded defect's own signature, scaled: a handful of scouting beacons and some stdout.
SUMMARY_SCOUTING_ONLY = """% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 50.00    0.000010           2         5           sendto
 50.00    0.000010           1        10           write
------ ----------- ----------- --------- --------- ----------------
100.00    0.000020                    15           total
"""


class parse_strace_counts_test(unittest.TestCase):
    """@brief The summary parser, which is what every verdict rests on."""

    def test_reads_both_column_widths(self):
        counts = syscall_guard.parse_strace_counts(SUMMARY_MIXED)
        self.assertEqual(counts["sendmsg"], 500)
        self.assertEqual(counts["sendto"], 300)
        self.assertEqual(syscall_guard.sends(counts), 800)

    def test_skips_header_rule_and_total(self):
        counts = syscall_guard.parse_strace_counts(SUMMARY_MIXED)
        self.assertNotIn("total", counts)
        self.assertNotIn("syscall", counts)

    def test_empty_summary_is_zero_sends(self):
        self.assertEqual(syscall_guard.sends(syscall_guard.parse_strace_counts(SUMMARY_EMPTY)), 0)

    def test_write_never_counts_toward_the_floor(self):
        """@brief stdout and a socket write are the same row in a `-c` summary."""
        counts = syscall_guard.parse_strace_counts(SUMMARY_SCOUTING_ONLY)
        self.assertEqual(counts["write"], 10)
        self.assertEqual(syscall_guard.sends(counts), 5)
        self.assertLess(syscall_guard.sends(counts), syscall_guard.DEFAULT_FLOOR)


@unittest.skipIf(shutil.which("strace") is None, "strace not installed")
class audit_test(unittest.TestCase):
    """@brief End-to-end verdicts against the real tracer, both directions."""

    def test_true_transmits_nothing(self):
        """@brief BREAK THE LINE: a process that never sends must not be reportable."""
        self.assertEqual(syscall_guard.audit(["/bin/true"], floor=50, label="t"), 1)

    def test_a_real_sender_passes(self):
        """@brief And the guard must not fire on a process that does reach the wire."""
        sender = ("import socket\n"
                  "s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n"
                  "for _ in range(200): s.sendto(b'x', ('127.0.0.1', 9))\n")
        rc = syscall_guard.audit([sys.executable, "-c", sender], floor=50, label="t")
        self.assertEqual(rc, 0)

    def test_below_floor_fails_even_though_it_sent(self):
        """@brief "It transmitted" is not the bar — "beyond scouting" is."""
        sender = ("import socket\n"
                  "s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n"
                  "for _ in range(5): s.sendto(b'x', ('127.0.0.1', 9))\n")
        rc = syscall_guard.audit([sys.executable, "-c", sender], floor=50, label="t")
        self.assertEqual(rc, 1)

    def test_missing_binary_is_unavailable_not_pass(self):
        """@brief An audit that could not run must never read as a clean bill of health."""
        rc = syscall_guard.audit(["/nonexistent/bench_binary"], floor=50, label="t")
        self.assertEqual(rc, 2)


class cli_test(unittest.TestCase):
    """@brief The exit status run_compose.sh branches on."""

    def test_exit_status_is_the_verdict(self):
        proc = subprocess.run([sys.executable, str(HERE / "syscall_guard.py"),
                               "--floor", "50", "--label", "t", "--", "/bin/true"],
                              capture_output=True, text=True, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("verdict=FAIL", proc.stdout)

    def test_no_command_is_an_error(self):
        proc = subprocess.run([sys.executable, str(HERE / "syscall_guard.py"), "--floor", "50"],
                              capture_output=True, text=True, check=False)
        self.assertNotEqual(proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
