"""@brief Tests for bench/store_prune.py (#2229)."""
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import store_prune as sp  # noqa: E402


def _store(n: int) -> dict:
    mk = lambda i: {"commit": {"id": f"c{i:04d}"}, "date": i, "benches": [{"name": "r", "value": i}]}
    return {"lastUpdate": 1, "repoUrl": "x",
            "entries": {"lat (smaller is better)": [mk(i) for i in range(n)],
                        "thr (bigger is better)": [mk(i) for i in range(n)]}}


class StorePrune(unittest.TestCase):
    def _dir(self, n):
        t = tempfile.TemporaryDirectory()
        self.addCleanup(t.cleanup)
        d = pathlib.Path(t.name)
        (d / "data.js").write_text(sp.render(_store(n)))
        return d

    def test_below_trigger_is_a_noop(self):
        d = self._dir(150)
        self.assertEqual(sp.prune(d, 120, 150), "no prune needed")
        self.assertEqual([p.name for p in d.iterdir()], ["data.js"])

    def test_prune_keeps_newest_and_archives_oldest(self):
        d = self._dir(151)
        sp.prune(d, 120, 150)
        live = sp.parse((d / "data.js").read_text())["entries"]
        arch = sp.parse((d / "data-archive-001.js").read_text())["entries"]
        for suite in live:
            self.assertEqual(len(live[suite]), 120)
            self.assertEqual(live[suite][0]["date"], 31)
            self.assertEqual(live[suite][-1]["date"], 150)
            self.assertEqual([e["date"] for e in arch[suite]], list(range(31)))

    def test_second_prune_writes_a_new_archive_file(self):
        d = self._dir(151)
        sp.prune(d, 120, 150)
        (d / "data.js").write_text(sp.render(_store(182)))
        sp.prune(d, 120, 150)
        self.assertTrue((d / "data-archive-002.js").exists())

    def test_live_store_still_feeds_the_readers(self):
        import store_guard as sg
        d = self._dir(200)
        sp.prune(d, 120, 150)
        doc = sg.parse_data_js((d / "data.js").read_text())
        self.assertEqual(len(next(iter(doc["entries"].values()))), 120)

    def test_guard(self):
        d = self._dir(3)
        f = d / "data.js"
        self.assertEqual(sp.guard(f, 60, 85), 0)
        self.assertEqual(sp.guard(f, 0, 0.0001), 1)


if __name__ == "__main__":
    unittest.main()
