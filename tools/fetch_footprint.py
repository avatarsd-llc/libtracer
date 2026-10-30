#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Fetch the size gates' newest main artifacts for the MCU footprint table (#2380).

The MCU lane's footprint rows are generated, never typed: `docs/_ext/footprint.py`
renders them from the JSON each size gate uploads. This script downloads, for each
(workflow, artifact) pair below, the artifact of that workflow's newest successful run on
main, and unpacks its JSON into the output directory (default
`docs/_generated/footprint`).

It degrades, never fails: no token, a run with no such artifact, an expired artifact or
an API error leaves that file absent, prints a `::notice::`, and the page shows the row
as not measured. A failure to fetch is not a failure of the docs.

    GITHUB_TOKEN=... python3 tools/fetch_footprint.py [--out DIR]
"""
from __future__ import annotations

import argparse
import io
import json
import os
import pathlib
import sys
import urllib.request
import zipfile

#: (workflow file, artifact name); each artifact holds one JSON of the same name.
ARTIFACTS = [
    ("footprint-cortexm0.yml", "footprint-cortexm0"),
    ("esp-idf.yml", "rv32-footprint"),
    ("esp-idf.yml", "footprint-esp32c6"),
]

API = "https://api.github.com"


class _NoAuthRedirect(urllib.request.HTTPRedirectHandler):
    """@brief Drop the token on a redirect: an artifact download redirects to blob storage,
    which refuses a request that carries GitHub's Authorization header."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None:
            new.remove_header("Authorization")
        return new


def _get(url: str, token: str) -> bytes:
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}",
                                               "Accept": "application/vnd.github+json"})
    with urllib.request.build_opener(_NoAuthRedirect).open(req, timeout=60) as resp:
        return resp.read()


def fetch(repo: str, token: str, out: pathlib.Path, get=_get) -> list[str]:
    """@brief Unpack each artifact's JSON into @p out; return the files written."""
    out.mkdir(parents=True, exist_ok=True)
    written: list[str] = []
    runs: dict[str, tuple[int, str] | None] = {}
    for workflow, name in ARTIFACTS:
        try:
            if workflow not in runs:
                listing = json.loads(get(f"{API}/repos/{repo}/actions/workflows/{workflow}/runs"
                                         "?branch=main&status=success&per_page=1", token))
                found = listing.get("workflow_runs") or []
                runs[workflow] = (found[0]["id"], found[0].get("head_sha", "")) if found else None
            if runs[workflow] is None:
                raise LookupError(f"no successful main run of {workflow}")
            run_id, head_sha = runs[workflow]
            arts = json.loads(get(f"{API}/repos/{repo}/actions/runs/{run_id}/artifacts"
                                  f"?name={name}", token)).get("artifacts") or []
            live = [a for a in arts if not a.get("expired")]
            if not live:
                raise LookupError(f"run {run_id} of {workflow} has no live {name} artifact")
            with zipfile.ZipFile(io.BytesIO(get(live[0]["archive_download_url"], token))) as z:
                payload = json.loads(z.read(f"{name}.json"))
            # An artifact that does not record its commit (esp_size_gate's) takes the run's.
            if not payload.get("commit") and head_sha:
                payload["commit"] = head_sha
            (out / f"{name}.json").write_text(json.dumps(payload, indent=2) + "\n")
            written.append(f"{name}.json")
        except Exception as exc:  # degrade: the page renders the row as not measured
            print(f"::notice::footprint: {name} not fetched ({exc})")
    return written


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    root = pathlib.Path(__file__).resolve().parent.parent
    ap.add_argument("--out", default=str(root / "docs" / "_generated" / "footprint"))
    args = ap.parse_args()
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    repo = os.environ.get("GITHUB_REPOSITORY", "avatarsd-llc/libtracer")
    if not token:
        print("::notice::footprint: no token; the MCU footprint rows render as not measured")
        return 0
    written = fetch(repo, token, pathlib.Path(args.out))
    print(f"footprint: fetched {len(written)} of {len(ARTIFACTS)} artifacts: {', '.join(written)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
