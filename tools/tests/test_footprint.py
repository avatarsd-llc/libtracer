#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the generated MCU footprint table (`docs/_ext/footprint.py`, #2380).

The MCU lane's footprint rows come from the size gates' own JSON artifacts, fetched into
the docs build, and are never typed. The fixtures are real nightly artifacts:
footprint-cortexm0.json (footprint-cortexm0.yml), rv32-footprint.json and
footprint-esp32c6.json (esp-idf.yml). These pin the row each one becomes, the row a missing
artifact becomes, and the unmeasured row a malformed artifact becomes.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import json
import os
import pathlib
import shutil
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "docs", "_ext"))

import footprint  # noqa: E402

sys.path.insert(0, os.path.join(REPO, "tools"))
import fetch_footprint  # noqa: E402

FIXTURES = pathlib.Path(REPO) / "tools" / "tests" / "fixtures" / "footprint"


class RowsTest(unittest.TestCase):
    def test_every_artifact_becomes_its_row(self):
        rows = footprint.rows(FIXTURES)
        self.assertEqual([r[0] for r in rows],
                         ["Cortex-M0", "RV32 (ESP32-C6 core)", "ESP32-C6, ESP-IDF"])
        m0, rv, esp = rows
        self.assertEqual(m0[1], "NARROW")
        self.assertEqual(m0[3], "14,192 B (16,384 B budget, 2,192 B left)")
        self.assertEqual(m0[4], "7,684 B")
        self.assertIn("13.2.1", m0[5])
        self.assertEqual(m0[6], "b529978b")
        self.assertEqual((rv[3], rv[4], rv[6]), ("38,611 B", "7,268 B", "6b6eba06"))
        self.assertEqual((esp[1], esp[3], esp[4]), ("MID", "178,098 B", "36,629 B"))
        self.assertEqual(esp[6], "not recorded")

    def test_a_missing_artifact_says_so_and_names_its_workflow(self):
        with tempfile.TemporaryDirectory() as d:
            shutil.copy(FIXTURES / "footprint-cortexm0.json", d)
            rows = footprint.rows(pathlib.Path(d))
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[0][3], "14,192 B (16,384 B budget, 2,192 B left)")
        for row in rows[1:]:
            self.assertEqual(row[3], "not measured in this build")
            self.assertIn("esp-idf.yml", row[5])

    def test_no_directory_at_all_is_three_unmeasured_rows(self):
        rows = footprint.rows(pathlib.Path("/nonexistent/footprint"))
        self.assertEqual([r[3] for r in rows], ["not measured in this build"] * 3)

    def test_a_malformed_artifact_renders_unmeasured_and_never_fails_the_build(self):
        for bad in (json.dumps({"mcpu": "x"}), "{not json", json.dumps([1, 2]),
                    json.dumps({"flash_bytes": "14192"}), b"\xff\xfe"):
            with tempfile.TemporaryDirectory() as d:
                path = pathlib.Path(d) / "footprint-cortexm0.json"
                path.write_bytes(bad if isinstance(bad, bytes) else bad.encode())
                shutil.copy(FIXTURES / "rv32-footprint.json", d)
                rows = footprint.rows(pathlib.Path(d))
            self.assertEqual((rows[0][3], rows[0][4]), (footprint.UNMEASURED,) * 2, bad)
            self.assertIn("unreadable", rows[0][5], bad)
            self.assertEqual(rows[1][3], "38,611 B", bad)



def _zip(name: str, payload: bytes) -> bytes:
    import io
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(name, payload)
    return buf.getvalue()


class FetchTest(unittest.TestCase):
    """The fetcher: newest successful main run per workflow, degrade on any gap."""

    def _api(self, expired=()):
        calls = []

        def get(url, token):
            calls.append(url)
            if "/workflows/" in url:
                wf = url.split("/workflows/")[1].split("/")[0]
                return json.dumps({"workflow_runs": [{"id": {"esp-idf.yml": 2}.get(wf, 1),
                                                       "head_sha": "feedface" * 5}]}).encode()
            if "/artifacts?name=" in url:
                name = url.split("name=")[1]
                return json.dumps({"artifacts": [{"expired": name in expired,
                                                  "archive_download_url": f"zip:{name}"}]}).encode()
            name = url.split("zip:")[1]
            return _zip(f"{name}.json", (FIXTURES / f"{name}.json").read_bytes())

        return get, calls

    def test_every_artifact_lands_and_each_workflow_is_listed_once(self):
        get, calls = self._api()
        with tempfile.TemporaryDirectory() as d:
            written = fetch_footprint.fetch("o/r", "t", pathlib.Path(d), get=get)
            rows = footprint.rows(pathlib.Path(d))
        expected = footprint.rows(FIXTURES)
        expected[2][6] = "feedface"  # the ESP artifact records no commit; the run's head stands in
        self.assertEqual(rows, expected)
        self.assertEqual(sorted(written), sorted(f"{n}.json" for _, n in fetch_footprint.ARTIFACTS))
        self.assertEqual(sum("/workflows/" in c for c in calls), 2)

    def test_an_expired_artifact_is_skipped_not_fatal(self):
        get, _ = self._api(expired={"rv32-footprint"})
        with tempfile.TemporaryDirectory() as d:
            written = fetch_footprint.fetch("o/r", "t", pathlib.Path(d), get=get)
            rows = footprint.rows(pathlib.Path(d))
        self.assertNotIn("rv32-footprint.json", written)
        self.assertEqual(rows[1][3], footprint.UNMEASURED)
        self.assertNotEqual(rows[0][3], footprint.UNMEASURED)

    def test_a_run_without_the_artifact_falls_back_to_an_older_successful_run(self):
        calls = []

        def get(url, token):
            calls.append(url)
            if "/workflows/" in url:
                page = int(url.split("page=")[-1])
                ids = {1: [10, 11], 2: [12, 13]}.get(page, [])
                return json.dumps({"workflow_runs": [{"id": i, "head_sha": f"{i:040d}"}
                                                     for i in ids]}).encode()
            if "/artifacts?name=" in url:
                run = int(url.split("/runs/")[1].split("/")[0])
                name = url.split("name=")[1]
                arts = [{"expired": False, "archive_download_url": f"zip:{name}"}] if run == 12 else []
                return json.dumps({"artifacts": arts}).encode()
            name = url.split("zip:")[1]
            return _zip(f"{name}.json", (FIXTURES / f"{name}.json").read_bytes())

        with tempfile.TemporaryDirectory() as d:
            written = fetch_footprint.fetch("o/r", "t", pathlib.Path(d), get=get)
            rows = footprint.rows(pathlib.Path(d))
        self.assertEqual(len(written), 3)
        self.assertEqual(rows[2][6], "00000000")  # the ESP commit comes from run 12, the one used
        self.assertFalse(any("/runs/13/" in c for c in calls))  # stops at the first hit

    def test_the_walk_back_is_bounded(self):
        calls = []

        def get(url, token):
            calls.append(url)
            if "/workflows/" in url:
                page = int(url.split("page=")[-1])
                return json.dumps({"workflow_runs": [{"id": page * 100 + k, "head_sha": "x"}
                                                     for k in range(5)]}).encode()
            return json.dumps({"artifacts": []}).encode()

        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(fetch_footprint.fetch("o/r", "t", pathlib.Path(d), get=get), [])
        listed = [c for c in calls if "/workflows/" in c]
        self.assertEqual(len(listed), 2 * fetch_footprint.MAX_PAGES)  # two workflows, each bounded

    def test_an_api_error_is_skipped_not_fatal(self):
        def get(url, token):
            raise OSError("network down")
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(fetch_footprint.fetch("o/r", "t", pathlib.Path(d), get=get), [])


if __name__ == "__main__":
    unittest.main()
