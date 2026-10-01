#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Gate every reference to the CI base image on the Dockerfile's content hash.

WHY THIS EXISTS. The `container:` jobs run in `ghcr.io/avatarsd-llc/libtracer-ci`,
built from `.github/ci-image/Dockerfile` by `ci-image.yml`. Its tag is
`noble-` + the first 12 hex chars of that file's sha256, so the tag names the exact
tool set. That only holds if every `image:` reference is bumped together with the
Dockerfile: a reference left on the old tag keeps running the OLD tool set while
the PR reads as if it changed it, and a reference to a tag nobody built fails at
image pull with nothing in the message to say why. This gate turns "bump every
reference in the same PR" from a convention into a rule.

Usage:
    python3 tools/check_ci_image_tag.py      # gate (exit 1 on drift)
"""
import hashlib
import re
import sys
from pathlib import Path

IMAGE = "ghcr.io/avatarsd-llc/libtracer-ci"
"""@brief Registry path of the CI base image, without a tag."""

DOCKERFILE = Path(".github/ci-image/Dockerfile")
"""@brief The image's single source of truth, relative to the repository root."""

REF_RE = re.compile(re.escape(IMAGE) + r":([A-Za-z0-9._-]+)")
"""@brief Any tagged reference to the image, anywhere in a workflow file."""


def expected_tag(dockerfile: bytes) -> str:
    """@brief The tag ci-image.yml publishes for this Dockerfile content.

    Must stay identical to ci-image.yml's `sha256sum … | cut -c1-12`.
    """
    return "noble-" + hashlib.sha256(dockerfile).hexdigest()[:12]


def check(root: Path) -> list:
    """@brief Return one human-readable error per drifted reference (empty = pass)."""
    tag = expected_tag((root / DOCKERFILE).read_bytes())
    errors = []
    for wf in sorted((root / ".github/workflows").glob("*.y*ml")):
        for lineno, line in enumerate(wf.read_text().splitlines(), 1):
            for found in REF_RE.findall(line):
                if found != tag:
                    errors.append(f"{wf.relative_to(root)}:{lineno}: {IMAGE}:{found} "
                                  f"(Dockerfile hash says {tag})")
    return errors


def main() -> int:
    """@brief CLI entry: print drift and exit 1, or confirm the tag and exit 0."""
    root = Path(__file__).resolve().parent.parent
    errors = check(root)
    for e in errors:
        print(f"::error::{e}")
    if errors:
        print(f"{len(errors)} reference(s) disagree with {DOCKERFILE}. Bump them to "
              f"{IMAGE}:{expected_tag((root / DOCKERFILE).read_bytes())}.")
        return 1
    print(f"OK: every reference is {IMAGE}:{expected_tag((root / DOCKERFILE).read_bytes())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
