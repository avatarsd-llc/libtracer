#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for `tools/sync-version.py`, the unified-lockstep version stamper.

The stamper is the only thing standing between a `vX.Y.Z` tag and five registries
disagreeing about what `X.Y.Z` contains, and its `--check` mode is the CI gate. A
gate is worth exactly what its failures are worth, so most of what is asserted here
is that `--check` **fails** on a tree it should reject — the mode that had never
been exercised on the npm lockfile and so silently passed for two releases (#862).

Every test runs the real CLI in a sandbox. `sync-version.py` derives its repo root
from its own `__file__`, so copying the script plus the files it stamps into a temp
directory reproduces the whole tool — argument parsing, exit codes and all — with no
risk of writing to the checked-out tree.

Run with ``python3 -m unittest discover -s tools/tests`` (no third-party deps).
"""
import difflib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(REPO, "tools", "sync-version.py")


def _load_stamper():
    """Import `sync-version.py` under a legal module name (the filename has a dash)."""
    spec = importlib.util.spec_from_file_location("sync_version", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sv = _load_stamper()

# Every file the stamper reads or writes, asked of the tool itself rather than
# hand-listed here: a sandbox built from a copy of this list cannot go stale when a
# sixth registry is added, and a test fixture that quietly stops covering an
# artifact is the failure mode this whole file exists to catch.
STAMPED = (
    [sv.VERSION_FILE]
    + [path for path, _pattern, _label in sv.CORE_MANIFESTS]
    + sv.TS_PACKAGES
    + [sv.TS_LOCKFILE, sv.RUST_CARGO]
)


def read(path):
    """Read a UTF-8 text file whole."""
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def write(path, text):
    """Write a UTF-8 text file whole."""
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def changed_lines(before, after):
    """How many lines differ between two texts, counted on both sides like `diff`."""
    old, new = before.split("\n"), after.split("\n")
    return sum(1 for line in difflib.unified_diff(old, new, n=0) if line[:1] in "+-" and line[:3] not in ("+++", "---"))


class SandboxTest(unittest.TestCase):
    """Base: a throwaway repo holding the real artifacts and the real script."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        for src in STAMPED:
            dst = os.path.join(self.tmp, str(src.relative_to(sv.ROOT)))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
        os.makedirs(os.path.join(self.tmp, "tools"), exist_ok=True)
        self.script = os.path.join(self.tmp, "tools", "sync-version.py")
        shutil.copy2(SCRIPT, self.script)
        self.lock = self.path("bindings/typescript/package-lock.json")
        self.version = read(self.path("VERSION")).strip()

    def path(self, rel):
        """Absolute path to `rel` inside the sandbox."""
        return os.path.join(self.tmp, rel)

    def stamp(self, *args):
        """Run the sandboxed CLI; returns the CompletedProcess (never raises)."""
        return subprocess.run(
            [sys.executable, self.script, *args], capture_output=True, text=True, cwd=self.tmp
        )

    def lock_entries(self):
        """The `packages/<dir>` objects of the sandbox lockfile, keyed by directory."""
        packages = json.loads(read(self.lock))["packages"]
        return {key: value for key, value in packages.items() if key.startswith("packages/")}

    def set_lock_version(self, old_version, stale):
        """Hand-revert the lockfile to `stale` — the drift the gate must see.

        Deliberately a blunt text substitution over the whole file rather than a
        reuse of the tool's own span logic: a fixture built with the code under
        test cannot notice that code looking in the wrong place.
        """
        text = read(self.lock)
        text = text.replace(f'"version": "{old_version}"', f'"version": "{stale}"')
        text = text.replace(sv.dep_range(old_version), sv.dep_range(stale))
        write(self.lock, text)


class CheckModeTest(SandboxTest):
    """`--check`, the CI gate — what it must reject."""

    def test_the_untouched_sandbox_passes(self):
        """Control: the copied tree is clean, so a later failure is the drift, not the copy."""
        result = self.stamp("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ok: every artifact matches", result.stdout)

    def test_check_fails_when_the_lockfile_drifts(self):
        """The #862 regression: a hand-reverted lockfile must fail the gate.

        Before the fix this exited 0 and printed `ok: every artifact matches VERSION`
        while the lockfile still pinned the previous release.
        """
        stale = "0.0.1"
        self.set_lock_version(self.version, stale)
        result = self.stamp("--check")
        self.assertEqual(result.returncode, 1, "the gate passed on a drifted lockfile:\n" + result.stdout)
        self.assertIn("ts lockfile", result.stdout)
        self.assertIn(stale, result.stdout)

    def test_check_names_every_drifted_lockfile_field(self):
        """All four workspace versions and all four internal ranges are reported."""
        self.set_lock_version(self.version, "0.0.1")
        result = self.stamp("--check")
        reported = [line for line in result.stdout.split("\n") if "ts lockfile" in line]
        self.assertEqual(len(reported), 8, "\n".join(reported))
        for key in self.lock_entries():
            self.assertTrue(any(key in line for line in reported), f"{key} unreported")

    def test_check_fails_on_a_drifted_range_alone(self):
        """A stale internal range with a correct version is still drift."""
        write(self.lock, read(self.lock).replace(sv.dep_range(self.version), ">=0.0.1 <0.1.0"))
        result = self.stamp("--check")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("ts lockfile", result.stdout)

    def test_check_is_not_fooled_by_a_stamped_manifest(self):
        """Manifests right + lockfile wrong is exactly the state that shipped twice."""
        self.set_lock_version(self.version, "0.0.1")
        manifests = self.stamp("--check").stdout
        self.assertNotIn("ts/core", manifests)
        self.assertIn("ts lockfile", manifests)


class StampModeTest(SandboxTest):
    """The rewrite — what it must move, and what it must leave alone."""

    def test_stamp_then_check_is_clean(self):
        """A bump followed by the gate: the whole point of the tool."""
        target = "9.9.9"
        stamped = self.stamp(target)
        self.assertEqual(stamped.returncode, 0, stamped.stdout + stamped.stderr)
        checked = self.stamp(target, "--check")
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_stamp_moves_every_lockfile_workspace_entry(self):
        """Each `packages/<dir>` version and internal range lands on the new value."""
        target = "9.9.9"
        self.stamp(target)
        entries = self.lock_entries()
        self.assertEqual(len(entries), len(sv.TS_PACKAGES))
        for key, entry in entries.items():
            self.assertEqual(entry["version"], target, key)
            for dep, rng in entry.get("peerDependencies", {}).items():
                if dep.startswith("@avatarsd-llc/"):
                    self.assertEqual(rng, sv.dep_range(target), f"{key} -> {dep}")

    def test_stamp_leaves_third_party_and_the_private_root_alone(self):
        """Only the workspace entries move; resolved deps and the 0.0.0 root do not."""
        before = json.loads(read(self.lock))
        self.stamp("9.9.9")
        after = json.loads(read(self.lock))
        self.assertEqual(before["version"], after["version"])
        self.assertEqual(after["packages"][""], before["packages"][""])
        for key, entry in before["packages"].items():
            if not key.startswith("packages/"):
                self.assertEqual(after["packages"][key], entry, key)

    def test_stamp_edits_exactly_the_drifted_lines(self):
        """Two diff lines per changed field and no more — npm's layout is preserved."""
        before = read(self.lock)
        self.stamp("9.9.9")
        # 4 workspace versions + 4 concrete internal ranges, counted from the tree.
        fields = sum(
            1 + sum(1 for d in entry.get("peerDependencies", {}) if d.startswith("@avatarsd-llc/"))
            for entry in self.lock_entries().values()
        )
        self.assertEqual(changed_lines(before, read(self.lock)), 2 * fields)

    def test_the_lockfile_mirrors_each_package_json(self):
        """npm's own invariant, asserted without a registry round-trip.

        `npm install --package-lock-only` copies a workspace manifest's version and
        dependency maps into its lockfile entry verbatim. If they already agree after
        our in-place rewrite, npm has nothing left to change — which is what makes a
        no-network stamp equivalent to regenerating the file.
        """
        self.stamp("9.9.9")
        for pkg in sv.TS_PACKAGES:
            key = pkg.parent.relative_to(sv.TS_ROOT).as_posix()
            manifest = json.loads(read(self.path(str(pkg.relative_to(sv.ROOT)))))
            entry = self.lock_entries()[key]
            self.assertEqual(entry["version"], manifest["version"], key)
            self.assertEqual(entry["name"], manifest["name"], key)
            for field in ("dependencies", "devDependencies", "peerDependencies"):
                self.assertEqual(entry.get(field, {}), manifest.get(field, {}), f"{key}.{field}")

    def test_stamp_is_idempotent(self):
        """Running it twice changes nothing the second time."""
        self.stamp("9.9.9")
        once = read(self.lock)
        second = self.stamp("9.9.9")
        self.assertEqual(read(self.lock), once)
        self.assertNotIn("ts lockfile", second.stdout)

    def test_a_lockfile_missing_a_workspace_entry_is_an_error(self):
        """A truncated lockfile fails loudly instead of being silently half-stamped."""
        data = json.loads(read(self.lock))
        del data["packages"]["packages/core"]
        write(self.lock, json.dumps(data, indent=2) + "\n")
        result = self.stamp("--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("packages/core", result.stdout + result.stderr)


class SpanTest(unittest.TestCase):
    """The two pure helpers, on inputs the real tree does not happen to contain."""

    def test_a_brace_inside_a_string_does_not_desynchronise_the_span(self):
        """Why the JSON decoder is used instead of counting braces."""
        text = '{\n  "packages": {\n    "packages/core": {\n      "x": "a{b}c",\n      "version": "1.0.0"\n    },\n    "after": {}\n  }\n}\n'
        (start, end) = sv.lock_entry_spans(text, ["packages/core"])["packages/core"]
        self.assertEqual(json.loads(text[start:end]), {"x": "a{b}c", "version": "1.0.0"})

    def test_the_resolved_value_is_not_mistaken_for_the_entry_key(self):
        """`"resolved": "packages/core"` is a value; only the key form is a span."""
        text = '{\n  "a": {\n    "resolved": "packages/core",\n    "link": true\n  },\n  "packages/core": {\n    "version": "1.0.0"\n  }\n}\n'
        (start, end) = sv.lock_entry_spans(text, ["packages/core"])["packages/core"]
        self.assertEqual(json.loads(text[start:end]), {"version": "1.0.0"})

    def test_workspace_links_are_never_rewritten(self):
        """`*` and `workspace:*` dev links are not published-facing."""
        text = '{"version": "1.0.0", "devDependencies": {"@avatarsd-llc/libtracer": "*", "@avatarsd-llc/libtracer-ws": "workspace:*"}}'
        self.assertEqual(sv.ts_edits(text, "1.0.0", ">=1.0.0 <2.0.0"), [])

    def test_edits_splice_back_in_text_order(self):
        """`splice` reassembles the body with every span replaced and nothing else."""
        text = '{"version": "1.0.0", "peerDependencies": {"@avatarsd-llc/libtracer": ">=1.0.0 <2.0.0"}}'
        edits = sv.ts_edits(text, "2.0.0", ">=2.0.0 <3.0.0")
        self.assertEqual(len(edits), 2)
        self.assertEqual(
            sv.splice(text, edits),
            '{"version": "2.0.0", "peerDependencies": {"@avatarsd-llc/libtracer": ">=2.0.0 <3.0.0"}}',
        )


class RepoTreeTest(unittest.TestCase):
    """The committed tree, as the release will actually find it."""

    def test_the_lockfile_is_in_the_gate_workflow_trigger(self):
        """A gate that never runs on the file it guards is the bug, one level up.

        `version-consistency.yml` filters on `paths:`, which is an allowlist: without
        the lockfile listed, a PR touching only the lockfile skips the job entirely
        and the drift lands unseen.
        """
        workflow = read(os.path.join(REPO, ".github", "workflows", "version-consistency.yml"))
        rel = str(sv.TS_LOCKFILE.relative_to(sv.ROOT))
        self.assertEqual(workflow.count(f"- {rel}"), 2, f"{rel} must be in both the push and pull_request paths")

    def test_every_stamped_artifact_exists(self):
        """The tool's file list matches the tree; a moved manifest fails here, not at a tag."""
        for path in STAMPED:
            self.assertTrue(path.exists(), path)


if __name__ == "__main__":
    unittest.main()
