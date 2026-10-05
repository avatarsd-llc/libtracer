#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""@brief Source-selection tests for the performance page's trend charts (#1769).

bench-local is the trend instrument: one pinned CPU, both arms of each paired point from
the same pass. These tests pin the three things that make it the page's default: the
selector opens on it (with hosted one click away, labelled as the portability envelope),
the zenoh / libtracer ratio is built from same-pass arms only, and rows above 1 KiB reach
the default payload.

    python3 bench/test_render_history.py    # or: python3 -m unittest discover -s bench
"""
from __future__ import annotations

import json
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import render_history as rh  # noqa: E402

JS = pathlib.Path(__file__).resolve().parent.parent / "docs" / "_static" / "perf_history.js"

SIZES = (64, 1024, 4096, 16384)


def _store() -> dict:
    """@brief A two-commit store carrying the paired zenoh/libtracer payload family.

    Every size above 1 KiB is present on both arms so a test can tell "dropped by the
    renderer" from "never recorded".
    """
    benches = []
    for size in SIZES:
        for eng in ("", "zenoh "):
            benches.append({"name": f"{eng}inproc {size}B/fan1/1ep p50 latency",
                            "value": 100.0 + size / 64, "unit": "ns", "extra": "h"})
    return {"entries": {"libtracer latency (ns, smaller is better)": [
        {"commit": {"id": c * 40, "message": f"commit {c}"}, "benches": benches}
        for c in ("a", "b")]}}


class _NoGit(unittest.TestCase):
    """@brief Stubs the git-backed annotations so the tests depend on the store alone."""

    def setUp(self):
        self._saved = (rh.instrument_annotations, rh.release_annotations)
        rh.instrument_annotations = lambda *a, **k: []
        rh.release_annotations = lambda *a, **k: []

    def tearDown(self):
        rh.instrument_annotations, rh.release_annotations = self._saved


class RatioIsSamePassOnly(_NoGit):
    """@brief The quotient is offered on a same-pass store and never on the hosted one."""

    def _chart(self, payload: dict, cid: str) -> dict:
        return next(c for c in payload["charts"] if c["id"] == cid)

    def test_same_pass_store_carries_the_ratio(self):
        out = rh.build(_store(), {}, same_pass=True)
        self.assertIn("ratio", self._chart(out, "vs-zenoh-payload"))

    def test_hosted_store_carries_no_ratio(self):
        out = rh.build(_store(), {}, same_pass=False)
        chart = self._chart(out, "vs-zenoh-payload")
        self.assertNotIn("ratio", chart)
        # The absolute lines themselves are still drawn; only the quotient is withheld.
        self.assertEqual(len(chart["series"]), 2 * len(SIZES))

    def test_html_blocks_build_hosted_without_ratio(self):
        blocks = rh.html_blocks(_store(), _store())
        block = blocks["dispatch"]
        hosted = json.loads(re.search(r'class="ph-data">(.*?)</script>', block).group(1))
        local = json.loads(re.search(r'class="ph-data-local">(.*?)</script>', block).group(1))
        self.assertFalse(any("ratio" in c for c in hosted["charts"]))
        self.assertTrue(any("ratio" in c for c in local["charts"]))


class BenchLocalIsTheDefault(_NoGit):
    """@brief The selector opens on bench-local; hosted is one click away and labelled."""

    def _buttons(self, block: str) -> list[tuple[str, str, str]]:
        return re.findall(r'<button type="button" class="ph-srcbtn( on)?" data-src="(\w+)"'
                          r'[^>]*>([^<]*)</button>', block)

    def test_local_button_is_first_and_selected(self):
        block = rh.html_blocks(_store(), _store())["dispatch"]
        btns = self._buttons(block)
        self.assertEqual([(b[0], b[1]) for b in btns], [(" on", "local"), ("", "hosted")])
        self.assertIn("portability envelope", btns[1][2])

    def test_hosted_is_selected_when_local_is_unreachable(self):
        block = rh.html_blocks(_store(), None)["dispatch"]
        self.assertIn('data-src="local" disabled', block)
        self.assertEqual([(b[0], b[1]) for b in self._buttons(block)],
                         [("", "local"), (" on", "hosted")])

    def test_client_default_is_local(self):
        js = JS.read_text()
        body = re.search(r"function readSource\(\) \{(.*?)\n  \}", js, re.S).group(1)
        self.assertIn('=== "hosted" ? "hosted" : "local"', body)
        self.assertIn('catch (e) { return "local"; }', body)


class OneCommitAxisPerStore(_NoGit):
    """@brief Every trend chart of a store shares one x-axis: one slot per commit (#1801).

    The renderer derives tick spacing from the axis length alone, so one shared `shas`
    list per store is what makes the domain and the ticks identical on every card.
    """

    @staticmethod
    def _drifted() -> dict:
        """@brief Suites that drifted apart: a re-run commit, and a run missing one suite."""
        def entry(c: str, run: int, metric: str) -> dict:
            benches = [{"name": f"{eng}inproc {size}B/fan1/1ep {metric}", "value": 100.0 + run}
                       for size in SIZES for eng in ("", "zenoh ")]
            return {"commit": {"id": c * 40, "message": f"commit {c}",
                               "timestamp": f"2026-10-0{'abc'.index(c) + 1}T00:00:00Z"},
                    "date": run, "benches": benches}
        lat = [entry("a", 0, "p50 latency"), entry("b", 1, "p50 latency"),
               entry("b", 2, "p50 latency"), entry("c", 3, "p50 latency")]
        thr = [entry("a", 0, "throughput"), entry("c", 3, "throughput")]
        return {"entries": {"hosted latency": lat, "hosted throughput": thr}}

    def _payloads(self) -> list[dict]:
        block = rh.html_blocks(self._drifted(), self._drifted())["dispatch"]
        return [json.loads(re.search(rf'class="{cls}">(.*?)</script>', block).group(1))
                for cls in ("ph-data", "ph-data-local")]

    def test_every_suite_shares_the_axis(self):
        for payload in self._payloads():
            axes = {tuple(s["shas"]) for s in payload["suites"].values()}
            self.assertEqual(axes, {("a" * 7, "b" * 7, "c" * 7)})
            for c in payload["charts"]:
                for v in c["metrics"]:
                    for se in v["series"]:
                        self.assertTrue(all(0 <= p[0] < 3 for p in se["pts"]))

    def test_missing_commit_is_a_gap_and_a_rerun_one_slot(self):
        payload = self._payloads()[0]
        chart = next(c for c in payload["charts"] if c["id"] == "vs-zenoh-payload")
        by = {v["name"]: v["series"][0]["pts"] for v in chart["metrics"]}
        self.assertEqual([p[0] for p in by["throughput"]], [0, 2])
        # The re-run of `b` keeps its last measurement in its one slot.
        self.assertEqual(by["p50 latency"][1], [1, 102.0])

    def test_points_carry_their_commit_date(self):
        suite = self._payloads()[0]["suites"]["latency"]
        self.assertEqual(suite["dates"], ["2026-10-01", "2026-10-02", "2026-10-03"])

    def test_sweeps_are_log2_with_a_tick_per_size(self):
        for fam in rh.FAMILIES:
            if "px" in fam:
                self.assertTrue(fam["px"]["log"], fam["id"])
        js = JS.read_text()
        for fn in ("renderSweep", "renderParam"):
            body = re.search(rf"function {fn}\(.*?\n  \}}\n", js, re.S).group(0)
            self.assertIn("Math.log2", body)
            self.assertNotIn("Math.log10(v) - Math.log10(xmin)", body)

    def test_trend_cards_have_a_recent_window_zoom(self):
        self.assertIn("ph-rlast", JS.read_text())


class RowsAboveOneKibAreVisible(_NoGit):
    """@brief Payload sizes above 1 KiB reach the default (bench-local) payload."""

    def test_every_size_survives(self):
        out = rh.build(_store(), {}, same_pass=True)
        chart = next(c for c in out["charts"] if c["id"] == "vs-zenoh-payload")
        pvs = sorted({s["rpv"] for s in chart["series"]})
        self.assertEqual(pvs, [float(s) for s in SIZES])


def _sweep_store() -> dict:
    """@brief Three commits of the banked fan and payload sweeps, both engines, both suites.

    The middle commit is stamped contaminated, the way the bench-local host guard stamps a
    sample measured under load, so a test can tell "picked" from "trusted".
    """
    def entry(c: str, k: int, metric: str, extra: str) -> dict:
        benches = []
        for eng in ("", "zenoh "):
            for fan in (1, 8):
                benches.append({"name": f"{eng}inproc 64B/fan{fan}/1ep {metric}",
                                "value": 100.0 * fan + k, "extra": extra})
            for size in (8, 1024):
                benches.append({"name": f"{eng}inproc {size}B/fan1/1ep {metric}",
                                "value": 1000.0 + k, "extra": extra})
        return {"commit": {"id": c * 40, "message": f"commit {c}"}, "benches": benches}

    def suite(metric: str) -> list[dict]:
        return [entry(c, k, metric, "h · CONTAMINATED (test)" if c == "b" else "h")
                for k, c in enumerate("abc")]
    return {"entries": {"bench-local latency": suite("p50 latency"),
                        "bench-local throughput": suite("throughput")}}


class ComparisonHistoryPicker(_NoGit):
    """@brief The zenoh comparison sweeps carry a history picker over bench-local (#1771)."""

    def setUp(self):
        super().setUp()
        rh.release_annotations = lambda entries: [{"i": 0, "label": "v0.16.0", "approx": False}]

    def _hist(self) -> dict:
        import render_compare as rc
        return rc.history(_sweep_store())

    def test_picks_carry_commits_and_release_tags(self):
        picks = self._hist()["picks"]
        self.assertEqual([p["sha"] for p in picks], ["a" * 7, "b" * 7, "c" * 7])
        self.assertEqual(picks[0]["rel"], "v0.16.0")
        self.assertNotIn("rel", picks[2])

    def test_contaminated_pass_is_not_pickable(self):
        line = self._hist()["charts"]["ltz-lat-fan"]["zenoh"]
        self.assertEqual(line["xs"], [1, 8])
        self.assertIsNone(line["v"][1])
        self.assertEqual(line["v"][2], [102.0, 802.0])

    def test_bandwidth_is_rate_times_size(self):
        line = self._hist()["charts"]["ltz-mb-size"]["libtracer"]
        # 64 B is the fan sweep's fan-1 point, which is also a payload-sweep point.
        self.assertEqual(line["xs"], [8, 64, 1024])
        self.assertEqual(line["v"][0], [1000.0 * 8 / 1e6, 100.0 * 64 / 1e6, 1000.0 * 1024 / 1e6])

    def test_every_banked_sweep_gets_the_picker(self):
        import render_compare as rc
        rows = rc.parse("\n".join(
            f"RESULT\t{s}\tinproc\t64\t{f}\t1\t1\t1\t1\t1\t1\t1"
            for s in ("libtracer", "zenoh") for f in (1, 8)))
        out = rc.build(rows, self._hist())
        self.assertEqual(len(out["hist"]["picks"]), 3)
        fan = next(c for c in out["charts"] if c["id"] == "ltz-lat-fan")
        self.assertEqual([h["key"] for h in fan["hist"]], ["libtracer", "zenoh"])
        # The banked line keeps the live line's legend text and color.
        live = {s["key"]: s for s in fan["series"]}
        self.assertEqual(fan["hist"][1]["label"], live["zenoh"]["label"])
        self.assertEqual(fan["hist"][1]["ci"], live["zenoh"]["ci"])

    def test_no_store_means_no_picker(self):
        import render_compare as rc
        self.assertIsNone(rc.history(None))
        rows = rc.parse("RESULT\tlibtracer\tinproc\t64\t1\t1\t1\t1\t1\t1\t1\t1")
        self.assertNotIn("hist", rc.build(rows, None))

    def test_renderer_draws_the_compare_pick_dashed(self):
        body = JS.read_text()
        self.assertIn("ph-cmpon", body)
        self.assertIn('stroke-dasharray="7 5"', body)


def _node() -> str | None:
    """@brief The node binary, or None: the band's arithmetic lives in the page script."""
    import shutil
    return shutil.which("node")


class NoiseBand(_NoGit):
    """@brief The trailing-window noise band (#1848): bench-local only, checked numerically.

    The band is computed client-side, because the reader picks the window. So the test runs
    the page script's own `windowBand` under node over a series with a known spread, rather
    than a Python copy of it that could drift from what the page draws.
    """

    def _band(self, pts: list, n: int) -> list:
        import subprocess
        node = _node()
        if node is None:
            self.skipTest("node is not installed; the band is computed by the page script")
        fn = re.search(r"\n  function windowBand\(.*?\n  \}\n", JS.read_text(), re.S).group(0)
        out = subprocess.run([node, "-e", fn + f"console.log(JSON.stringify(windowBand({json.dumps(pts)}, {n})));"],
                             capture_output=True, text=True, check=True, timeout=30)
        return json.loads(out.stdout)

    def test_band_matches_a_known_spread(self):
        # Values 1..10 repeating: every 10-point window holds exactly {1, ..., 10}, so
        # p10 = 1.9, p90 = 9.1 (linear interpolation), min 1, max 10, mean 5.5, and the
        # sample standard deviation is sqrt(82.5 / 9).
        pts = [[i, float(i % 10 + 1)] for i in range(30)]
        band = self._band(pts, 10)
        self.assertEqual([t[0] for t in band], list(range(9, 30)))
        cv = (82.5 / 9) ** 0.5 / 5.5
        for t in band:
            for got, want in zip(t[1:], (1.9, 9.1, 1.0, 10.0, cv)):
                self.assertAlmostEqual(got, want, places=9)

    def test_constant_series_has_zero_width(self):
        band = self._band([[i, 42.0] for i in range(12)], 10)
        self.assertEqual(len(band), 3)
        self.assertTrue(all(t[1:] == [42.0, 42.0, 42.0, 42.0, 0] for t in band))

    def test_short_series_has_no_band(self):
        self.assertEqual(self._band([[i, 1.0 + i] for i in range(9)], 10), [])

    def test_window_counts_recorded_points_not_slots(self):
        # Slots 3 and 4 were omitted (a contaminated sample is), so the window ending at
        # slot 6 reaches back over five recorded points, not five slots.
        pts = [[0, 1.0], [1, 2.0], [2, 3.0], [5, 4.0], [6, 5.0]]
        band = self._band(pts, 5)
        self.assertEqual([t[0] for t in band], [6])
        self.assertEqual(band[0][3:5], [1.0, 5.0])

    def test_bands_are_bench_local_only(self):
        block = rh.html_blocks(_store(), _store())["dispatch"]
        hosted = json.loads(re.search(r'class="ph-data">(.*?)</script>', block).group(1))
        local = json.loads(re.search(r'class="ph-data-local">(.*?)</script>', block).group(1))
        self.assertTrue(local.get("bands"))
        self.assertNotIn("bands", hosted)

    def test_default_window_and_tooltip_cv(self):
        js = JS.read_text()
        self.assertEqual(int(re.search(r"var BAND_N = (\d+);", js).group(1)), rh.BAND_N)
        self.assertEqual(rh.BAND_N, 10)
        self.assertIn("var bands = !!D.bands && !param", js)
        # The tooltip row carries the window's CV.
        self.assertIn("bt = bandAt[si] && bandAt[si][i]", js)
        self.assertIn("cv ' + fmtPct(bt[5])", js)


if __name__ == "__main__":
    unittest.main()
