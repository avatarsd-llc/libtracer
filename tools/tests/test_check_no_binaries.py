#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the committed-binary gate.

An unverified gate is how #1356 happened in the first place: 34 ELFs passed every
check in the repo. So this suite drives BOTH arms of `tools/check_no_binaries.py`
against real throwaway git repositories — real commits, real `git diff --numstat`
output — rather than against a mocked diff:

* the RED arm: an ELF committed under a build-tree-shaped path must fail;
* the GREEN arm: a conformance vector must pass, because the vector suite is the one
  legitimate producer of binary content in this tree;
* the evasions: an ELF renamed to `input.bin` under a vector path must still fail
  (magic beats the allowlist), a `.bin` outside the vector layout gets no exemption,
  and a `.gitattributes` line forcing a path to diff as text must not silence it.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import os
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_no_binaries as cnb  # noqa: E402

# A minimal but genuine ELF header — enough magic for both git and the gate to agree.
ELF_BYTES = b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 8 + b"\x02\x00\x3e\x00" + b"\x00" * 64

# What a real vector holds: a TLV frame — a type byte, a length, and a payload with
# the NUL bytes a fixed-width length field produces. Small, binary, and legitimate.
VECTOR_BYTES = bytes([0x21, 0x08, 0x00, 0x00, 0x00, 0x2A, 0xDE, 0xAD, 0x00, 0xEF])


class GitFixture:
    """A throwaway repository with a `base` commit and a `topic` branch on top."""

    def __init__(self, tmpdir):
        self.path = tmpdir
        self._git("init", "-q", "-b", "main")
        self._git("config", "user.email", "gate@example.invalid")
        self._git("config", "user.name", "Gate Test")
        self.write("README.md", b"# fixture\n")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "base")
        self._git("checkout", "-q", "-b", "topic")

    def _git(self, *args):
        subprocess.run(
            ["git", "-C", self.path] + list(args),
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def write(self, relpath, content):
        full = os.path.join(self.path, relpath)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as handle:
            handle.write(content)

    def commit(self, message="topic"):
        self._git("add", "-A")
        self._git("commit", "-q", "-m", message)

    def offenders(self):
        return cnb.check("main", "topic", repo=self.path)


class GateArms(unittest.TestCase):
    """The two arms the definition of done names: red on an ELF, green on a vector."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = GitFixture(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_committed_elf_fails(self):
        # The #1356 shape: a build tree the ignore rules did not anticipate.
        self.repo.write("bench/out/bench_path_label", ELF_BYTES)
        self.repo.commit()
        offenders = self.repo.offenders()
        self.assertEqual([p for p, _ in offenders], ["bench/out/bench_path_label"])
        self.assertIn("ELF", offenders[0][1])

    def test_conformance_vector_passes(self):
        self.repo.write(
            "tests/conformance/vectors/v1/framing/pl-bit-opaque/input.bin",
            VECTOR_BYTES,
        )
        self.repo.write(
            "tests/conformance/vectors/v1/framing/pl-bit-opaque/expected.json",
            b'{"ok": true}\n',
        )
        self.repo.commit()
        self.assertEqual(self.repo.offenders(), [])

    def test_reject_vector_passes(self):
        self.repo.write(
            "tests/conformance/vectors/v1/framing/reserved-bit0-reject/reject.bin",
            VECTOR_BYTES,
        )
        self.repo.commit()
        self.assertEqual(self.repo.offenders(), [])

    def test_ordinary_source_change_passes(self):
        self.repo.write("core/src/graph.cpp", b"int main() { return 0; }\n")
        self.repo.commit()
        self.assertEqual(self.repo.offenders(), [])


class Evasions(unittest.TestCase):
    """The ways an extension allowlist or an attribute could be used to slip past."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = GitFixture(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_elf_wearing_a_vector_path_still_fails(self):
        # Magic is tested BEFORE the allowlist precisely for this case.
        self.repo.write(
            "tests/conformance/vectors/v1/framing/sneaky/input.bin", ELF_BYTES
        )
        self.repo.commit()
        offenders = self.repo.offenders()
        self.assertEqual(len(offenders), 1)
        self.assertIn("ELF", offenders[0][1])

    def test_bin_outside_the_vector_layout_is_not_exempt(self):
        self.repo.write("firmware/blob.bin", VECTOR_BYTES)
        self.repo.commit()
        offenders = self.repo.offenders()
        self.assertEqual([p for p, _ in offenders], ["firmware/blob.bin"])

    def test_bin_nested_at_the_wrong_depth_is_not_exempt(self):
        self.repo.write(
            "tests/conformance/vectors/v1/framing/case/extra/input.bin", VECTOR_BYTES
        )
        self.repo.commit()
        self.assertEqual(len(self.repo.offenders()), 1)

    def test_gitattributes_cannot_launder_a_binary(self):
        # Forcing `diff` on the path makes git's numstat report line counts, so the
        # numstat verdict alone would clear it; the NUL scan is the backstop.
        self.repo.write(".gitattributes", b"payload.dat diff\n")
        self.repo.write("payload.dat", b"lots\x00of\x00nuls\n" * 10)
        self.repo.commit()
        offenders = dict(self.repo.offenders())
        self.assertIn("payload.dat", offenders)
        self.assertIn("NUL", offenders["payload.dat"])

    def test_static_library_and_object_shapes_fail(self):
        self.repo.write("out/libtracer.a", b"!<arch>\n" + b"\x00" * 32)
        self.repo.write("out/graph.o", ELF_BYTES)  # a .o is an ELF relocatable
        self.repo.commit()
        self.assertEqual(
            sorted(p for p, _ in self.repo.offenders()),
            ["out/graph.o", "out/libtracer.a"],
        )

    def test_extensionless_binary_with_no_known_magic_still_fails(self):
        # Neither an extension nor a recognised magic — only the content test can
        # convict here, and this is the shape the #1356 build tree actually had.
        self.repo.write("cmake-build-debug/mystery", b"\x93\x00\x11\xff" * 64)
        self.repo.commit()
        offenders = self.repo.offenders()
        self.assertEqual([p for p, _ in offenders], ["cmake-build-debug/mystery"])
        self.assertIn("binary content", offenders[0][1])

    def test_short_artifact_with_no_nul_is_caught_by_its_name(self):
        # Reads as text to git and to the NUL scan; the name is the only tell.
        self.repo.write("build/empty.o", b"stub\n")
        self.repo.commit()
        offenders = self.repo.offenders()
        self.assertEqual([p for p, _ in offenders], ["build/empty.o"])
        self.assertIn("named like build output", offenders[0][1])

    def test_high_entropy_artifact_with_no_nul_still_fails(self):
        # 200 bytes of noise with no zero byte in them: git calls it text and so does
        # the NUL scan. The UTF-8 decode is what convicts.
        blob = bytes((i * 7 + 129) % 255 + 1 for i in range(200))
        self.assertNotIn(0, blob)
        self.repo.write("out/mystery", blob)
        self.repo.commit()
        offenders = self.repo.offenders()
        self.assertEqual([p for p, _ in offenders], ["out/mystery"])
        self.assertIn("UTF-8", offenders[0][1])

    def test_non_ascii_source_is_not_mistaken_for_a_binary(self):
        # The tree is full of em dashes and arrows; the decode must not red them.
        self.repo.write("docs/prose.md", "— → ✅ ≥ « » 日本語\n".encode("utf-8"))
        self.repo.commit()
        self.assertEqual(self.repo.offenders(), [])

    def test_oversize_text_file_fails(self):
        self.repo.write("docs/dump.txt", b"a" * (cnb.MAX_TEXT_BYTES + 1))
        self.repo.commit()
        offenders = self.repo.offenders()
        self.assertEqual([p for p, _ in offenders], ["docs/dump.txt"])
        self.assertIn("ceiling", offenders[0][1])

    def test_oversize_vector_fails(self):
        self.repo.write(
            "tests/conformance/vectors/v1/framing/huge/input.bin",
            b"\x21" * (cnb.VECTOR_MAX_BYTES + 1),
        )
        self.repo.commit()
        offenders = self.repo.offenders()
        self.assertEqual(len(offenders), 1)
        self.assertIn("vector ceiling", offenders[0][1])


class DiffPlumbing(unittest.TestCase):
    """`git diff --numstat -z` parsing — renames emit an extra pair of path fields."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = GitFixture(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_rename_is_judged_on_the_post_image_path(self):
        # Land a legitimate vector on `main`, then move it out of the vector layout on
        # the topic branch: the post-image has no exemption, so the move must fail.
        self.repo._git("checkout", "-q", "main")
        self.repo.write(
            "tests/conformance/vectors/v1/framing/moved/input.bin", VECTOR_BYTES
        )
        self.repo.commit("vector")
        self.repo._git("checkout", "-q", "topic")
        self.repo._git("merge", "-q", "main")
        self.repo._git(
            "mv", "tests/conformance/vectors/v1/framing/moved/input.bin", "moved.bin"
        )
        self.repo.commit("move it out")
        self.assertEqual([p for p, _ in self.repo.offenders()], ["moved.bin"])

    def test_deletions_are_ignored(self):
        self.repo._git("rm", "-q", "README.md")
        self.repo.commit("drop it")
        self.assertEqual(self.repo.offenders(), [])


class ThisRepo(unittest.TestCase):
    """The gate's own subject: libtracer's committed conformance vectors are legal."""

    def test_every_tracked_vector_is_accepted(self):
        tracked = subprocess.run(
            ["git", "-C", REPO, "ls-files", "-z", "tests/conformance/vectors"],
            check=True,
            stdout=subprocess.PIPE,
        ).stdout.decode()
        bins = [p for p in tracked.split("\0") if p.endswith(".bin")]
        self.assertGreater(len(bins), 50, "the vector suite should be substantial")
        unmatched = [p for p in bins if not cnb.is_vector_path(p)]
        self.assertEqual(unmatched, [], "vector layout drifted from the allowlist")

    def test_no_tracked_file_would_trip_the_gate(self):
        """Zero false positives: re-adding the whole tree today must stay green.

        The heuristics are aggressive on purpose, so the standing proof that they are
        not TOO aggressive is the tree itself — every one of its ~1000 tracked files,
        judged as if this change had just added it.
        """
        tracked = subprocess.run(
            ["git", "-C", REPO, "ls-files", "-z"],
            check=True,
            stdout=subprocess.PIPE,
        ).stdout.decode("utf-8", "surrogateescape")
        offenders = []
        checked = 0
        for path in tracked.split("\0"):
            full = os.path.join(REPO, path)
            if not path or not os.path.isfile(full):
                continue
            with open(full, "rb") as handle:
                blob = handle.read()
            checked += 1
            reason = cnb.classify(path, blob, git_says_binary=False)
            if reason is not None:
                offenders.append("{}: {}".format(path, reason))
        self.assertGreater(checked, 500, "the sweep should see the whole tree")
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
