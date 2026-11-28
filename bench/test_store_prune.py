"""@brief Tests for bench/store_prune.py (#2229, #2939)."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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

    def test_loader_concatenates_archives_and_live(self):
        import store_load
        d = self._dir(200)
        sp.prune(d, 40, 50)
        doc = store_load.load_path(d / "data.js")
        for ents in doc["entries"].values():
            self.assertEqual([e["date"] for e in ents], list(range(200)))

    def test_loader_dedupes_a_crashed_prune(self):
        import store_load
        d = self._dir(200)
        sp.prune(d, 40, 50)
        (d / "data.js").write_text(sp.render(_store(200)))  # live still holds everything
        doc = store_load.load_path(d / "data.js")
        for ents in doc["entries"].values():
            self.assertEqual(len(ents), 200)

    def test_existing_archive_is_never_overwritten(self):
        d = self._dir(151)
        (d / "data-archive-001.js").write_text("sentinel")
        sp.prune(d, 120, 150)
        self.assertEqual((d / "data-archive-001.js").read_text(), "sentinel")
        self.assertTrue((d / "data-archive-002.js").exists())

    def test_archives_split_by_bytes(self):
        d = self._dir(151)
        old = sp.ARCHIVE_MAX_BYTES
        sp.ARCHIVE_MAX_BYTES = 4_000
        self.addCleanup(setattr, sp, "ARCHIVE_MAX_BYTES", old)
        sp.prune(d, 120, 150)
        arch = sorted(d.glob("data-archive-*.js"))
        self.assertGreater(len(arch), 1)
        self.assertTrue(all(a.stat().st_size < 4_000 + 2_000 for a in arch))

    def test_guard(self):
        d = self._dir(3)
        f = d / "data.js"
        self.assertEqual(sp.guard(f, 60, 85), 0)
        self.assertEqual(sp.guard(f, 0, 0.0001), 1)


def _sh(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, check=True, text=True, capture_output=True).stdout


class StoreSync(unittest.TestCase):
    """@brief `sync` against a scratch gh-pages remote, with another writer forced to win the push race."""

    D = "dev/bench"

    def setUp(self):
        t = tempfile.TemporaryDirectory()
        self.addCleanup(t.cleanup)
        root = pathlib.Path(t.name)
        env = {f"GIT_{w}_{k}": v for w in ("AUTHOR", "COMMITTER") for k, v in (("NAME", "t"), ("EMAIL", "t@t"))}
        patch = mock.patch.dict(os.environ, env)
        patch.start()
        self.addCleanup(patch.stop)
        self.url = (root / "remote.git").as_uri()
        _sh(root, "init", "-q", "--bare", "remote.git")
        seed = root / "seed"
        _sh(root, "init", "-q", "-b", "gh-pages", "seed")
        (seed / self.D).mkdir(parents=True)
        for n in (7, 8):  # two commits, so the shallow fetch has a grafted root to trip on
            (seed / self.D / "data.js").write_text(sp.render(_store(n)))
            _sh(seed, "add", ".")
            _sh(seed, "commit", "-qm", f"bank {n}")
        _sh(seed, "push", "-q", self.url, "gh-pages")
        self.work, self.other = root / "work", root / "other"
        _sh(root, "clone", "-q", self.url, "work")
        _sh(root, "clone", "-q", "-b", "gh-pages", self.url, "other")
        cwd = os.getcwd()
        os.chdir(self.work)
        self.addCleanup(os.chdir, cwd)
        self.wt = root / "wt"

    def _bank_elsewhere(self):
        """@brief Another writer (benchmark-action) appends a point to the unpruned store and pushes."""
        _sh(self.other, "pull", "-q", "--rebase", "origin", "gh-pages")
        f = self.other / self.D / "data.js"
        doc = sp.parse(f.read_text())
        for ents in doc["entries"].values():
            ents.append({"commit": {"id": "rival"}, "date": 999, "benches": []})
        f.write_text(sp.render(doc))
        _sh(self.other, "commit", "-qam", "rival point")
        _sh(self.other, "push", "-q", "origin", "HEAD:gh-pages")
        return _sh(self.other, "rev-parse", "HEAD").strip()

    def _sync(self, races, **kw):
        """@brief Run sync; before each of its first @p races pushes, the rival pushes first."""
        real, rivals = sp._git, []

        def git(*args, **k):
            if args[0] == "push" and len(rivals) < races:
                rivals.append(self._bank_elsewhere())
            return real(*args, **k)

        args = dict(keep=4, trigger=6, attempts=3, warn_mb=60, fail_mb=85) | kw
        with mock.patch.object(sp, "_git", git):
            rc = sp.sync(self.D, self.wt, "prune", **args)
        return rc, rivals

    def _remote(self, name):
        _sh(self.work, "fetch", "-q", "origin", "gh-pages")
        return _sh(self.work, "show", f"FETCH_HEAD:{self.D}/{name}")

    def test_no_race_prunes_and_pushes(self):
        rc, _ = self._sync(0)
        self.assertEqual(rc, 0)
        for ents in sp.parse(self._remote("data.js"))["entries"].values():
            self.assertEqual([e["date"] for e in ents], [4, 5, 6, 7])
        self.assertFalse(self.wt.exists())

    def test_lost_race_refetches_and_reprunes(self):
        rc, rivals = self._sync(2)
        self.assertEqual((rc, len(rivals)), (0, 2))
        _sh(self.work, "merge-base", "--is-ancestor", rivals[-1], "FETCH_HEAD")  # rival kept
        live = sp.parse(self._remote("data.js"))["entries"]
        arch = sp.parse(self._remote("data-archive-001.js"))["entries"]
        for suite, ents in live.items():
            self.assertEqual([e["date"] for e in ents], [6, 7, 999, 999])
            self.assertEqual([e["date"] for e in arch[suite]], [0, 1, 2, 3, 4, 5])
        with self.assertRaises(subprocess.CalledProcessError):  # one archive: no stale prune landed
            self._remote("data-archive-002.js")
        self.assertFalse(self.wt.exists())

    def test_every_race_lost_guards_the_remote_store(self):
        unpruned = len(sp.render(_store(8)).encode()) / 1e6
        rc, rivals = self._sync(9, fail_mb=unpruned * 0.9)  # a pruned local copy would pass
        self.assertEqual((rc, len(rivals)), (1, 3))
        self.assertGreater(len(sp.parse(self._remote("data.js"))["entries"]["thr (bigger is better)"]), 6)
        self.assertFalse(self.wt.exists())


if __name__ == "__main__":
    unittest.main()
