#!/usr/bin/env python3
"""@brief The commit the ratchet compares HEAD against: the last BENCHED ancestor (#2466).

perf.yml's push runs share one concurrency group per branch, so a landing batch of N
merges benches once: GitHub keeps one pending run and supersedes the older pending ones.
The ratchet therefore cannot compare HEAD with `HEAD^`. A superseded commit between them
was never measured, and a regression it carried would read as the baseline. The baseline
is the newest commit whose own perf.yml push run SUCCEEDED and which is an ancestor of
HEAD: everything between it and HEAD, superseded members included, is inside this one
comparison, so nothing goes unmeasured.

    ratchet_baseline.py --repo OWNER/NAME --head SHA [--workflow perf.yml]

Prints the baseline sha, or nothing when no benched ancestor is found or the API cannot
be read; the caller then falls back to the first parent and says so. A run that failed or
was cancelled is not a baseline: it may not have measured, and skipping it only widens the
comparison. HEAD's own run is skipped too, so the nightly compares HEAD with the commit
benched before it, as its push did. Stdlib only; the token is `GITHUB_TOKEN`.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request
from typing import Callable, Iterable

API = "https://api.github.com"
MAX_CANDIDATES = 30  # recent successful runs checked; none an ancestor -> the caller falls back, loudly


def pick(head: str, benched: Iterable[str], is_ancestor: Callable[[str], bool]) -> str:
    """@brief The first sha of @p benched (newest first) that is a proper ancestor of @p head.

    @p benched lists the head shas of successful push runs, newest first. A sha that is
    HEAD itself, a repeat, or not an ancestor (a re-run of an older commit, another
    branch) is skipped. Returns "" when none qualifies within MAX_CANDIDATES.
    """
    seen: set[str] = set()
    for sha in benched:
        if sha == head or sha in seen:
            continue
        seen.add(sha)
        if len(seen) > MAX_CANDIDATES:
            break
        if is_ancestor(sha):
            return sha
    return ""


def _get(path: str, token: str) -> dict:
    req = urllib.request.Request(API + path, headers={
        "Accept": "application/vnd.github+json",
        "Authorization": f"Bearer {token}",
        "X-GitHub-Api-Version": "2022-11-28",
    })
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--head", required=True)
    ap.add_argument("--workflow", default="perf.yml")
    args = ap.parse_args(argv)
    token = os.environ.get("GITHUB_TOKEN", "")
    try:
        runs = _get(f"/repos/{args.repo}/actions/workflows/{args.workflow}/runs"
                    f"?event=push&status=success&per_page=100", token)["workflow_runs"]
        benched = [r["head_sha"] for r in runs]

        def is_ancestor(sha: str) -> bool:
            # `ahead`: HEAD is ahead of sha, so sha is HEAD's ancestor.
            return _get(f"/repos/{args.repo}/compare/{sha}...{args.head}",
                        token)["status"] == "ahead"

        base = pick(args.head, benched, is_ancestor)
    except Exception as e:  # noqa: BLE001 - any API failure means "fall back", loudly
        print(f"::warning::ratchet baseline lookup failed ({e}); the caller falls back",
              file=sys.stderr)
        return 0
    if base:
        print(base)
    return 0


if __name__ == "__main__":
    sys.exit(main())
