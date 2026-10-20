#!/usr/bin/env python3
"""@brief The one loader for the bench-local history: `data-archive-*.js` plus `data.js` (#2229).

`store_prune.py` keeps the live `data.js` small and moves older entries into
`data-archive-NNN.js` beside it. Every reader that needs more than the live window (the
rolling baseline needs 21 TRUSTED points, which is about the newest 100 entries on the
real store; the results page draws long-range charts) loads through here, so none of them
can silently see only the live tail.

    store_load.py mirror --ref FETCH_HEAD --dir dev/bench-local --out DIR

`mirror` copies `data.js` and every archive from a git ref into DIR (stale archives in DIR
are removed first), so a reader given `DIR/data.js` finds its archives next to it.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys

_ARCHIVE = re.compile(r"^data-archive-\d{3}\.js$")


def parse(text: str) -> dict:
    """@brief Parse a `window.BENCHMARK_DATA = {...}` assignment (ValueError if truncated)."""
    start = text.find("{")
    if start < 0:
        raise ValueError("no JSON object in data.js")
    return json.loads(text[start:].rstrip().rstrip(";").rstrip())


def _key(entry: dict) -> tuple:
    return (str((entry.get("commit") or {}).get("id", "")), entry.get("date"))


def merge(live: dict, archives: list[dict]) -> dict:
    """@brief @p live with the @p archives' entries prepended per series, oldest first.

    An entry present twice (a prune that crashed between its two writes) is kept once.
    """
    if not archives:
        return live
    out = dict(live)
    ents: dict[str, list] = {}
    for doc in [*archives, live]:
        for suite, es in (doc.get("entries") or {}).items():
            have = {_key(e) for e in ents.get(suite, [])}
            ents.setdefault(suite, []).extend(e for e in es if _key(e) not in have)
    out["entries"] = ents
    return out


def load_path(path: str | pathlib.Path) -> dict:
    """@brief `data.js` at @p path merged with the `data-archive-*.js` files beside it."""
    p = pathlib.Path(path)
    arch = sorted(q for q in p.parent.iterdir() if _ARCHIVE.match(q.name))
    return merge(parse(p.read_text()), [parse(q.read_text()) for q in arch])


def _git_show(ref: str, path: str, cwd: pathlib.Path | None) -> str | None:
    p = subprocess.run(["git", "show", f"{ref}:{path}"], capture_output=True, text=True, cwd=cwd)
    return p.stdout if p.returncode == 0 and p.stdout else None


def load_git(ref: str, store: str, cwd: pathlib.Path | None = None) -> dict | None:
    """@brief The store at `ref:store` (a `.../data.js` path) with its archives, or None."""
    raw = _git_show(ref, store, cwd)
    if raw is None:
        return None
    d = store.rsplit("/", 1)[0] if "/" in store else ""
    ls = subprocess.run(["git", "ls-tree", "--name-only", ref, f"{d}/" if d else ""],
                        capture_output=True, text=True, cwd=cwd)
    names = sorted(n for n in ls.stdout.split() if _ARCHIVE.match(n.rsplit("/", 1)[-1]))
    docs = [parse(t) for n in names if (t := _git_show(ref, n, cwd))]
    return merge(parse(raw), docs)


def mirror(ref: str, store_dir: str, out: pathlib.Path) -> int:
    """@brief Copy `data.js` and the archives of @p store_dir at @p ref into @p out."""
    out.mkdir(parents=True, exist_ok=True)
    for q in out.iterdir():
        if _ARCHIVE.match(q.name):
            q.unlink()
    ls = subprocess.run(["git", "ls-tree", "--name-only", ref, f"{store_dir}/"],
                        capture_output=True, text=True)
    names = [n for n in ls.stdout.split()
             if n.endswith("/data.js") or _ARCHIVE.match(n.rsplit("/", 1)[-1])]
    if not any(n.endswith("/data.js") for n in names):
        (out / "data.js").write_text("")
        return 0
    for n in names:
        (out / n.rsplit("/", 1)[-1]).write_text(_git_show(ref, n, None) or "")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("mirror")
    m.add_argument("--ref", required=True)
    m.add_argument("--dir", required=True)
    m.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    return mirror(a.ref, a.dir, pathlib.Path(a.out))


if __name__ == "__main__":
    sys.exit(main())
