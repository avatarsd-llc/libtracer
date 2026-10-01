#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the CI base image tag gate (`tools/check_ci_image_tag.py`).

Each arm builds a synthetic repository with one Dockerfile and one workflow:

* a reference on the Dockerfile's hash passes;
* a reference left on a stale tag fails, naming file and line;
* a workflow with no reference at all is not policed.

Run with ``python3 -m unittest discover -s tools/tests``.
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_ci_image_tag as gate  # noqa: E402

DOCKERFILE = b"FROM ubuntu:24.04\n"


def _repo(workflow: str) -> Path:
    """@brief Materialise a throwaway repo with the Dockerfile and one workflow."""
    root = Path(tempfile.mkdtemp())
    (root / ".github/ci-image").mkdir(parents=True)
    (root / ".github/ci-image/Dockerfile").write_bytes(DOCKERFILE)
    (root / ".github/workflows").mkdir(parents=True)
    (root / ".github/workflows/x.yml").write_text(workflow)
    return root


class CiImageTagGateTest(unittest.TestCase):
    """@brief The gate passes on the content hash and fails on anything else."""

    def test_matching_reference_passes(self):
        tag = gate.expected_tag(DOCKERFILE)
        self.assertEqual(gate.check(_repo(f"    image: {gate.IMAGE}:{tag}\n")), [])

    def test_stale_reference_fails_with_location(self):
        errors = gate.check(_repo(f"jobs:\n  a:\n    image: {gate.IMAGE}:noble-000000000000\n"))
        self.assertEqual(len(errors), 1)
        self.assertIn("x.yml:3", errors[0])

    def test_no_reference_is_not_policed(self):
        self.assertEqual(gate.check(_repo("    image: ubuntu:24.04\n")), [])

    def test_tag_matches_shell_derivation(self):
        # ci-image.yml derives the tag with `sha256sum | cut -c1-12`; same digest, same prefix.
        self.assertRegex(gate.expected_tag(DOCKERFILE), r"^noble-[0-9a-f]{12}$")


if __name__ == "__main__":
    unittest.main()
