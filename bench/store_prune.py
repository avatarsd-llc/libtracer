#!/usr/bin/env python3
"""@brief Keep the bench-local history store under GitHub's 100 MB file limit (#2229).

`dev/bench-local/data.js` grows by one entry per series per banked point. At roughly
0.26 MB per point (both suites) it passed 100 MB after 361 points and every gh-pages push
was rejected. This tool bounds the live store and moves older entries into an archive
beside it, so nothing is deleted.

    store_prune.py prune --dir D [--keep 40] [--trigger 50]
    store_prune.py guard --data F [--warn-mb 60] [--fail-mb 85]

**Window.** The live store keeps the newest `--keep` entries of every series. A point is
now about 0.9 MB (both suites), so 40 points is about 37 MB. Every in-repo reader needs
less: the rolling baseline (#1301) reads 20 trusted points plus the newest, step
detection reads the last 5, and the results page charts and pickers read the live store.
40 leaves the baseline two windows deep for contaminated points that `trusted()` drops.

**Hysteresis.** Pruning only starts once a series holds more than `--trigger` entries
(about 46 MB at the peak), then cuts back to `--keep`: one gh-pages commit per ~10 points.

**Archive.** Each prune writes its evicted entries to a NEW `data-archive-NNN.js`, the
same `window.BENCHMARK_DATA = {...}` shape as `data.js`, oldest range first. Files are
never rewritten, so gh-pages history grows by the evicted slice only, and no archive
file can approach the limit.

**Guard.** `guard` exits 1 above `--fail-mb` and prints a warning above `--warn-mb`, so
the next breach is a named failure before the push, not a `GH001` after it.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

PREFIX = "window.BENCHMARK_DATA = "
ARCHIVE_SLICE = 100
_ARCHIVE = re.compile(r"^data-archive-(\d{3})\.js$")


def parse(text: str) -> dict:
    """@brief Parse a `data.js` assignment into its JSON document (ValueError if truncated)."""
    start = text.find("{")
    if start < 0:
        raise ValueError("no JSON object in data.js")
    return json.loads(text[start:].rstrip().rstrip(";").rstrip())


def render(doc: dict) -> str:
    """@brief Serialise a store the way benchmark-action does (two-space JSON)."""
    return PREFIX + json.dumps(doc, indent=2) + "\n"


def _key(entry: dict) -> tuple:
    return (str((entry.get("commit") or {}).get("id", "")), entry.get("date"))


def prune_doc(doc: dict, keep: int, trigger: int) -> tuple[dict, dict[str, list[dict]]]:
    """@brief Split @p doc into (live, evicted-per-suite); no-op until a suite passes @p trigger."""
    entries = doc.get("entries") or {}
    if not any(len(v) > trigger for v in entries.values()):
        return doc, {}
    live, gone = dict(doc), {}
    live["entries"] = {}
    for suite, ents in entries.items():
        cut = max(0, len(ents) - keep)
        live["entries"][suite], gone[suite] = ents[cut:], ents[:cut]
    return live, gone


def _next_archive(d: pathlib.Path) -> pathlib.Path:
    nums = [int(m.group(1)) for p in d.iterdir() if (m := _ARCHIVE.match(p.name))]
    return d / f"data-archive-{max(nums, default=0) + 1:03d}.js"


def prune(d: pathlib.Path, keep: int, trigger: int) -> str:
    """@brief Prune `d/data.js`, writing the evicted entries to a new archive file."""
    data = d / "data.js"
    doc = parse(data.read_text())
    live, gone = prune_doc(doc, keep, trigger)
    if not gone:
        return "no prune needed"
    # Archive first: a crash between the writes leaves a duplicate range, never a gap.
    # At most ARCHIVE_SLICE entries per series per file keeps every file far below the limit
    # even on the first prune of a long-grown store.
    names = []
    for lo in range(0, max(len(v) for v in gone.values()), ARCHIVE_SLICE):
        arch = _next_archive(d)
        adoc = {k: v for k, v in doc.items() if k != "entries"}
        adoc["entries"] = {k: v[lo:lo + ARCHIVE_SLICE] for k, v in gone.items()}
        arch.write_text(render(adoc))
        names.append(arch.name)
    data.write_text(render(live))
    moved = sum(len(v) for v in gone.values())
    return f"moved {moved} entries to {', '.join(names)}; live {data.stat().st_size / 1e6:.1f} MB"


def guard(path: pathlib.Path, warn_mb: float, fail_mb: float) -> int:
    """@brief Exit status for the store's size: 1 over @p fail_mb, 0 otherwise."""
    mb = path.stat().st_size / 1e6 if path.exists() else 0.0
    if mb > fail_mb:
        print(f"::error::{path} is {mb:.1f} MB, over the {fail_mb:g} MB guard (GitHub rejects "
              f"files over 100 MB). Lower --keep in bench/store_prune.py's workflow step.")
        return 1
    if mb > warn_mb:
        print(f"::warning::{path} is {mb:.1f} MB, over the {warn_mb:g} MB warning bar; "
              f"GitHub rejects files over 100 MB.")
    else:
        print(f"{path}: {mb:.1f} MB (warn {warn_mb:g}, fail {fail_mb:g})")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prune")
    p.add_argument("--dir", required=True)
    p.add_argument("--keep", type=int, default=40)
    p.add_argument("--trigger", type=int, default=50)
    g = sub.add_parser("guard")
    g.add_argument("--data", required=True)
    g.add_argument("--warn-mb", type=float, default=60.0)
    g.add_argument("--fail-mb", type=float, default=85.0)
    a = ap.parse_args(argv)
    if a.cmd == "prune":
        if a.keep < 1 or a.trigger < a.keep:
            ap.error("need 1 <= keep <= trigger")
        print(prune(pathlib.Path(a.dir), a.keep, a.trigger))
        return 0
    return guard(pathlib.Path(a.data), a.warn_mb, a.fail_mb)


if __name__ == "__main__":
    sys.exit(main())
