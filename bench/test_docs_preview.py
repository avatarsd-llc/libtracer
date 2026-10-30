#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""@brief Tests for the docs build's PR preview mode (#2348).

A preview skips what only renders a verdict another workflow gates on, and says so on the
page; ctest runs once per docs job and the performance page's rollup reads its JUnit file.

    python3 bench/test_docs_preview.py
"""
from __future__ import annotations

import importlib
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import gen_test_report as tr  # noqa: E402


def _load(preview: bool):
    """@brief Reload both generators with the preview switch set as the job sets it."""
    env = {"LIBTRACER_DOCS_PREVIEW": "1" if preview else ""}
    with mock.patch.dict(os.environ, env):
        importlib.reload(tr)
        import gen_results_page as g
        return importlib.reload(g)


class PreviewMode(unittest.TestCase):
    def tearDown(self):
        _load(False)

    def test_a_preview_runs_no_cross_core_driver(self):
        g = _load(True)
        with mock.patch.object(g.subprocess, "run") as run:
            summary, _ = g.cross_core_block()
        run.assert_not_called()
        self.assertIn("not run on PR previews", summary)

    def test_a_preview_tests_rollup_says_so_without_ctest(self):
        g = _load(True)
        with mock.patch.object(tr.subprocess, "run") as run:
            self.assertEqual(g.tests_block(), tr.PREVIEW_NOTE)
        run.assert_not_called()

    def test_a_preview_codec_block_runs_only_the_cpp_core(self):
        g = _load(True)
        called = []
        with mock.patch.object(g, "_codec_ts", side_effect=lambda: called.append("ts")), \
             mock.patch.object(g, "_codec_rust", side_effect=lambda: called.append("rust")), \
             mock.patch.object(g, "_codec_cpp", return_value=(None, "not built")):
            out = g.codec_block()
        self.assertEqual(called, [])
        self.assertIn("ts-core, rust-core: not run on PR previews", out)

    def test_the_rollup_reads_the_junit_file_and_does_not_rerun_ctest(self):
        _load(False)
        with tempfile.TemporaryDirectory() as d:
            build = pathlib.Path(d)
            (build / "ctest-junit.xml").write_text(
                '<testsuite><testcase name="a_test" time="0.5" status="run"/></testsuite>')
            with mock.patch.object(tr.subprocess, "run") as run:
                results = tr.run_ctest_junit(build, run=False)
            run.assert_not_called()
            self.assertEqual([r[0] for r in results], ["a_test"])


if __name__ == "__main__":
    unittest.main()
