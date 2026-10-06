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
import host_guard as hg  # noqa: E402

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


class ZenohCardsStateMatchedSemantics(_NoGit):
    """@brief Every libtracer-vs-Zenoh card pairs its rows and says what it holds equal
    (#1809): the topic arms are paired by spelling, the by-path arm against a put by key."""

    TOPICS = (1, 100, 10000)

    def _store(self) -> dict:
        benches = [{"name": f"{eng}{mode} 64B/fan1/{ep}ep p50 latency", "value": 50.0 + ep,
                    "unit": "ns", "extra": "h"}
                   for eng in ("", "zenoh ")
                   for mode in ("inproc-path", "topics-bound", "topics-addr")
                   for ep in self.TOPICS]
        return {"entries": {"libtracer latency (ns, smaller is better)": [
            {"commit": {"id": c * 40, "message": f"commit {c}"}, "benches": benches}
            for c in ("a", "b")]}}

    def test_topic_cards_pair_each_spelling(self):
        charts = {c["id"]: c for c in rh.build(self._store(), {}, same_pass=True)["charts"]}
        for cid in ("vs-zenoh-path", "vs-zenoh-topics-bound", "vs-zenoh-topics-addr"):
            chart = charts[cid]
            self.assertIn("ratio", chart, cid)
            arms = {(s["arm"], s["rk"]) for s in chart["series"]}
            self.assertEqual(arms, {(a, f"{ep} topics") for a in ("num", "den")
                                    for ep in self.TOPICS}, cid)

    def test_every_zenoh_card_states_the_match(self):
        cards = [f for f in rh.FAMILIES if f["id"].startswith("vs-zenoh")]
        self.assertGreaterEqual(len(cards), 5)
        for fam in cards:
            for clause in ("one fresh process per engine and family", "the same CPU pin",
                           "equal payload bytes", "resolution against resolution"):
                self.assertIn(clause, fam["cond"], fam["id"])


class RowsAboveOneKibAreVisible(_NoGit):
    """@brief Payload sizes above 1 KiB reach the default (bench-local) payload."""

    def test_every_size_survives(self):
        out = rh.build(_store(), {}, same_pass=True)
        chart = next(c for c in out["charts"] if c["id"] == "vs-zenoh-payload")
        pvs = sorted({s["rpv"] for s in chart["series"]})
        self.assertEqual(pvs, [float(s) for s in SIZES])


class LadderFamiliesAreCharted(_NoGit):
    """@brief Every #1806 ladder and cliff family charts its rows above 1 KiB, the gated
    16 KiB point included, one line per size."""

    LADDER = (64, 1024, 4096, 16384, 65536)

    def test_each_family_has_a_line_per_size(self):
        modes = {"eptype-stream-payload": "eptype-stream", "demux-value": "fwd-demux-value",
                 "cliff-heap": "cliff-alloc-heap", "cliff-pool": "cliff-alloc-pool",
                 "compact-forward": "compact-forward", "borrow-payload": "inproc-borrow",
                 "payload": "inproc"}
        benches = [{"name": f"{mode} {size}B/fan1/1ep p50 latency", "value": 10.0 + size / 64,
                    "unit": "ns", "extra": "h"}
                   for mode in modes.values() for size in self.LADDER]
        store = {"entries": {"libtracer latency (ns, smaller is better)": [
            {"commit": {"id": c * 40, "message": f"commit {c}"}, "benches": benches}
            for c in ("a", "b")]}}
        charts = {c["id"]: c for c in rh.build(store, {}, same_pass=True)["charts"]}
        for fam in modes:
            self.assertIn(fam, charts, f"{fam} draws no chart")
            pvs = sorted({s["pv"] for s in charts[fam]["series"]})
            self.assertEqual(pvs, [float(s) for s in self.LADDER], fam)


def _sweep_store(moved: float = 2.0) -> dict:
    """@brief Three commits of the banked fan and payload sweeps, both engines, both suites.

    The middle commit is stamped contaminated, the way the bench-local host guard stamps a
    sample measured under load, and every one of its values is scaled by @p moved, so a
    test can tell "picked" from "trusted". At the default 2x every row is an outlier
    against its neighbours and the whole pass is hidden; at 1.0 it agrees with them and
    is drawn (#1890).
    """
    def entry(c: str, k: int, metric: str, extra: str, m: float) -> dict:
        benches = []
        for eng in ("", "zenoh "):
            for fan in (1, 8):
                benches.append({"name": f"{eng}inproc 64B/fan{fan}/1ep {metric}",
                                "value": (100.0 * fan + k) * m, "extra": extra})
            for size in (8, 1024):
                benches.append({"name": f"{eng}inproc {size}B/fan1/1ep {metric}",
                                "value": (1000.0 + k) * m, "extra": extra})
        return {"commit": {"id": c * 40, "message": f"commit {c}"}, "benches": benches}

    def suite(metric: str) -> list[dict]:
        return [entry(c, k, metric, "h · CONTAMINATED (test)" if c == "b" else "h",
                      moved if c == "b" else 1.0)
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

    def test_flagged_pass_that_agrees_with_its_neighbours_is_pickable(self):
        import render_compare as rc
        line = rc.history(_sweep_store(moved=1.0))["charts"]["ltz-lat-fan"]["zenoh"]
        self.assertEqual(line["v"][1], [101.0, 801.0])

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


class PerRowTrust(_NoGit):
    """@brief A flagged run hides only its untrusted rows, not every row (#1890).

    The rule lives in `host_guard.untrusted_cells`; these cases pin it through the
    renderer's own entry points, which is what the page draws.
    """

    ROWS = [f"inproc {s}B/fan1/1ep p50 latency" for s in (1, 8, 64, 1024)]

    def _entries(self, flagged: dict[int, dict[str, float]], n: int = 9,
                 row_flag: set[str] | None = None) -> list[dict]:
        """@brief @p n runs of four quiet rows (100 ns, +-1%); @p flagged maps a run index
        to the values it carries instead, and stamps that run contaminated."""
        out = []
        for i in range(n):
            extra = "h · CONTAMINATED (A/A bracket 20.0% > 6.0% band)" if i in flagged else "h"
            benches = []
            for r in self.ROWS:
                v = flagged.get(i, {}).get(r, 100.0 + (i % 3) - 1)
                tok = f" · {hg.ROW_TOKEN}" if i in flagged and r in (row_flag or ()) else ""
                benches.append({"name": r, "value": v, "extra": extra + tok})
            out.append({"commit": {"id": str(i) * 40}, "benches": benches})
        return out

    def _drawn(self, entries: list[dict]) -> dict[str, list[int]]:
        series = rh._series_by_name(entries, rh._untrusted_cells(entries))
        return {name: [p[0] for p in pts] for name, pts in series.items()}

    def test_only_the_outlier_row_of_a_flagged_run_is_hidden(self):
        drawn = self._drawn(self._entries({4: {self.ROWS[2]: 160.0}}))
        self.assertNotIn(4, drawn[self.ROWS[2]])
        for r in (self.ROWS[0], self.ROWS[1], self.ROWS[3]):
            self.assertIn(4, drawn[r], f"{r} agreed with its neighbours and must be drawn")

    def test_a_row_that_failed_its_own_aa_check_stays_hidden(self):
        """In band against its neighbours, but its own A/A pair disagreed: hidden."""
        drawn = self._drawn(self._entries({4: {}}, row_flag={self.ROWS[1]}))
        self.assertNotIn(4, drawn[self.ROWS[1]])
        self.assertIn(4, drawn[self.ROWS[0]])

    def test_a_run_that_moved_as_a_whole_is_hidden_whole(self):
        moved = {r: 150.0 for r in self.ROWS[:3]}
        entries = self._entries({4: moved})
        cells = rh._untrusted_cells(entries)
        self.assertIsNone(cells[4][1], "3 of 4 rows moved: the run measured the machine")
        self.assertTrue(all(4 not in idx for idx in self._drawn(entries).values()))

    def test_a_noisy_row_is_judged_against_its_own_spread(self):
        """A bimodal row swings 100 <-> 130 by nature; a flagged 130 is not an outlier."""
        entries = self._entries({4: {self.ROWS[3]: 130.0}})
        for i, e in enumerate(entries):
            if i != 4:
                e["benches"][3]["value"] = 130.0 if i % 2 else 100.0
        self.assertIn(4, self._drawn(entries)[self.ROWS[3]])

    def test_an_unflagged_run_is_never_reclassified(self):
        entries = self._entries({})
        entries[4]["benches"][0]["value"] = 500.0
        self.assertEqual(rh._untrusted_cells(entries), {})

    def test_a_reviewed_sample_stays_hidden_whole(self):
        entries = self._entries({})
        known = {entries[4]["commit"]["id"][:8]: "reviewed"}
        cells = hg.untrusted_cells(entries, known)
        self.assertEqual(cells, {4: ("reviewed", None)})

    def test_payload_counts_the_partly_drawn_run(self):
        entries = self._entries({4: {self.ROWS[2]: 160.0}})
        out = rh.build({"entries": {"bench-local latency": entries}}, colors={})
        self.assertEqual(out["suites"]["latency"]["partial"], {"4": 1})
        self.assertEqual(out["suites"]["latency"]["contaminated"], {})


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



class GapMarkers(unittest.TestCase):
    """@brief A commit with no trusted value is drawn as missing, not as nothing (#1890)."""

    def _marks(self, pts: list) -> str:
        import subprocess
        node = _node()
        if node is None:
            self.skipTest("node is not installed; the markers are drawn by the page script")
        fn = re.search(r"\n  function gapMarks\(.*?\n  \}\n", JS.read_text(), re.S).group(0)
        call = (f"console.log(gapMarks({{label: 'fan 1', pts: {json.dumps(pts)}}}, "
                "function (i) { return i * 10; }, function (v) { return v; }, 'red', "
                "{shas: ['a', 'b', 'c', 'd', 'e', 'f']}));")
        return subprocess.run([node, "-e", fn + call], capture_output=True, text=True,
                              check=True, timeout=30).stdout

    def test_each_missing_slot_inside_the_span_gets_a_marker(self):
        out = self._marks([[0, 10.0], [3, 40.0], [4, 50.0]])
        self.assertEqual(out.count('class="ph-gapdot"'), 2)
        self.assertEqual(out.count('class="ph-gap"'), 1)
        self.assertIn("b: no trusted value", out)
        # The ring sits on the bridge between the recorded points either side.
        self.assertIn('cx="10.0" cy="20.0"', out)

    def test_no_marker_without_a_gap_or_outside_the_span(self):
        self.assertEqual(self._marks([[2, 1.0], [3, 2.0], [4, 3.0]]).strip(), "")

    def test_trend_view_draws_them(self):
        self.assertIn("s += gapMarks(se, X, Y, cc, suite);", JS.read_text())


if __name__ == "__main__":
    unittest.main()
