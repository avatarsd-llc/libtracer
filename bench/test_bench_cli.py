#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The argv contract of `bench_libtracer`: an unrecognised mode must REFUSE to run.

This exists because of #1040, which is a measurement defect and not a CLI nit. The mode
dispatch was a chain of `if (argv[1] == "...") { ...; return 0; }` tests with no
terminating else, so any other first argument fell through into the FULL default sweep
and exited 0. The isolated modes exist for A/B runs; an A/B run builds one arm from a
different commit; a mode added after that commit does not exist in it. The result was a
candidate that ran one axis for seconds against a baseline that ran every axis for
minutes, both emitting well-formed `RESULT` rows under the same `(mode, size, fan, ep)`
keys for a harness to join on. A typo (`taget`) had the same effect.

The binary under test comes from ``LIBTRACER_BENCH`` when set (the CI spelling — a path
that does not exist is then a FAILURE, never a skip), otherwise from the conventional
``bench/build/bench_libtracer``, and the tests skip only in that second case. One test
reads the mode table out of ``bench_libtracer.cpp`` **in this checkout**, so point
``LIBTRACER_BENCH`` at a binary built from this tree, not at another commit's.

    cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release
    cmake --build bench/build --target bench_libtracer -j
    python3 bench/test_bench_cli.py     # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import os
import pathlib
import re
import subprocess
import unittest

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_BENCH = HERE / "build" / "bench_libtracer"
SOURCE = HERE / "bench_libtracer.cpp"

# Two budgets, because the two shapes need opposite things.
#
# REFUSE_S bounds a run that must NOT sweep, so it has to sit well UNDER a default sweep
# (~27 s on this host, minutes on a 2-vCPU runner) or the pre-fix behaviour would slip
# under it and pass. A refusal returns in milliseconds, so 20 s is all slack.
#
# RUN_S bounds a run that legitimately measures. `lkv` is the cheapest mode (~0.03 s
# here), but a small shared runner is not this host: perf_gate.py allows 120 s for the
# same invocation, and this file has no reason to be tighter than the gate it guards.
REFUSE_S = 20
RUN_S = 120


def bench_binary() -> pathlib.Path:
    """@brief The binary under test, or a skip when no build is present."""
    env = os.environ.get("LIBTRACER_BENCH")
    if env:
        path = pathlib.Path(env)
        if not path.exists():
            raise AssertionError(f"LIBTRACER_BENCH={env} does not exist — build it first")
        return path
    if not DEFAULT_BENCH.exists():
        raise unittest.SkipTest(f"{DEFAULT_BENCH} not built (cmake --build bench/build "
                                "--target bench_libtracer)")
    return DEFAULT_BENCH


def result_rows(stdout: str) -> list:
    """@brief The `RESULT` rows in a transcript — the stream every consumer joins on."""
    return [ln for ln in stdout.splitlines() if ln.startswith("RESULT")]


def _partial(exc: subprocess.TimeoutExpired) -> str:
    """@brief Whatever a killed run had already written. `TimeoutExpired.stdout` is
    documented as bytes but is str under `text=True` and a list of chunks on some paths,
    and a crash HERE would replace the failure message that explains the timeout."""
    raw = exc.stdout or ""
    if isinstance(raw, list):
        raw = b"".join(raw) if raw and isinstance(raw[0], bytes) else "".join(raw)
    return raw.decode(errors="replace") if isinstance(raw, bytes) else raw


class UnknownModeIsRefused(unittest.TestCase):
    """@brief An argument that names no mode must not run a sweep (#1040)."""

    # `""` is in here because an empty argv[1] is a real shape: `$MODE` unset in a driver
    # script expands to it, and it matched no branch of the old chain either.
    BAD = ["bogus-mode-name", "taget", "fann", "--help", "grid ", "", "GRID"]

    def test_unknown_modes_exit_nonzero_with_no_result_row(self):
        bench = bench_binary()
        for arg in self.BAD:
            with self.subTest(arg=arg):
                try:
                    out = subprocess.run([str(bench), arg], capture_output=True, text=True,
                                         timeout=REFUSE_S)
                except subprocess.TimeoutExpired as exc:
                    self.fail(f"{bench.name} {arg!r} was still running after {REFUSE_S}s "
                              f"and had already emitted {len(result_rows(_partial(exc)))} "
                              "RESULT rows — it fell through into the default sweep (#1040)")
                self.assertEqual(result_rows(out.stdout), [],
                                 f"{arg!r} names no mode yet produced RESULT rows")
                self.assertNotEqual(out.returncode, 0,
                                    f"{arg!r} names no mode yet exited 0")


class UsageMatchesTheDispatchTable(unittest.TestCase):
    """@brief The listed modes and the dispatchable modes are one list, not two.

    The rejection must SAY what the modes are, or the next typo is a guess — and the list
    it prints must be the list it dispatches, which is what makes "a mode was added to the
    dispatch but not to the usage text" a failure here rather than a silence.
    """

    def test_usage_list_equals_the_source_table(self):
        bench = bench_binary()
        out = subprocess.run([str(bench), "no-such-mode"], capture_output=True, text=True,
                             timeout=REFUSE_S)
        printed = set()
        for line in out.stderr.splitlines():
            if line.lstrip().startswith("modes:"):
                names = line.split(":", 1)[1].split("(")[0]
                printed = {n.strip() for n in names.split("|") if n.strip()}
        self.assertEqual(printed, source_modes(),
                         "the printed mode list and the kModes table have diverged — a mode "
                         "that dispatches but is not listed is how #1040's silence returns")


class RecognisedModeStillRuns(unittest.TestCase):
    """@brief The positive control: the refusal above must not be refusing everything.

    Without this, `assertEqual(result_rows(...), [])` would also pass against a binary that
    cannot emit a row at all — the vacuous-guard shape. `lkv` is the cheapest real mode
    (~0.03 s, 8 rows), so this costs nothing and proves the assertion has teeth.
    """

    def test_lkv_mode_emits_rows_and_announces_itself(self):
        bench = bench_binary()
        out = subprocess.run([str(bench), "lkv"], capture_output=True, text=True,
                             timeout=RUN_S)
        self.assertEqual(out.returncode, 0, out.stderr[-400:])
        self.assertGreater(len(result_rows(out.stdout)), 0,
                           "a recognised mode produced no RESULT row")
        self.assertIn("MODE lkv", out.stderr.splitlines()[0] if out.stderr else "",
                      "an isolated mode must announce itself on stderr before its first row")


def source_modes() -> set:
    """@brief The mode names in `bench_libtracer.cpp`'s `kModes` table."""
    text = SOURCE.read_text(encoding="utf-8")
    table = re.search(r"constexpr bench_mode_t kModes\[\] = \{(.*?)\n\};", text, re.S)
    if table is None:
        raise AssertionError(f"no kModes table found in {SOURCE} — this test has rotted")
    names = set(re.findall(r'\{"([^"]+)",', table.group(1)))
    if not names:
        raise AssertionError(f"kModes table in {SOURCE} parsed as empty — this test has rotted")
    return names


if __name__ == "__main__":
    unittest.main(verbosity=2)
