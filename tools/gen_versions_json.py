#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate the docs version manifest (versions.json) at the built-site root.

The version switcher (docs/_static/version-switcher.js) reads this manifest to
populate its picker. To stay HONEST, the manifest lists only versions whose
subtree is *actually present* in the built site — it scans the output directory
for ``vMAJOR.MINOR.PATCH`` subdirectories rather than trusting a git tag list, so
a listed version is always a page that exists. ``latest`` (the current build,
served at the site root) is always first.

Usage:  tools/gen_versions_json.py <built-site-dir>

Run in docs.yml after the Sphinx build (and after any released-version subtrees
have been assembled into the output). Until a release deploys a ``vX.Y.Z/`` tree,
the manifest is just ``latest`` and the switcher renders a single-version chip.
"""
import json
import pathlib
import re
import sys

VERSION_DIR = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_versions_json.py <built-site-dir>")
    out = pathlib.Path(sys.argv[1])
    if not out.is_dir():
        sys.exit(f"error: not a directory: {out}")

    found = []
    for child in out.iterdir():
        m = VERSION_DIR.match(child.name)
        if child.is_dir() and m:
            found.append((tuple(int(g) for g in m.groups()), child.name))
    # Newest release first.
    found.sort(reverse=True)

    versions = [{"name": "latest (main)", "slug": "latest", "path": ""}]
    for _, name in found:
        versions.append({"name": name, "slug": name, "path": name + "/"})

    manifest = {"latest": "latest", "versions": versions}
    (out / "versions.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"versions.json: {', '.join(v['slug'] for v in versions)}")


if __name__ == "__main__":
    main()
