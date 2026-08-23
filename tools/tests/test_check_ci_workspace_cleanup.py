#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Unit tests for the #1538 workspace-ownership gate.

`tools/check_ci_workspace_cleanup.py` is a gate whose whole value is that it goes
RED on the shapes that actually occurred. Two of thirteen self-hosted container
jobs had silently lost the cleanup step, and one of the two is what left 129
root-owned paths under `core/build-minimal` and took `perf-local / bench-local`
red on 2026-08-23. A gate that only ever runs against a tree it already passes is
indistinguishable from a gate that reads nothing, so each arm below drives it over
a synthetic workflow directory built to exhibit one failure:

* the missing step — the real defect;
* the step present but NOT last, so a later step can re-dirty what it handed back;
* `if: success()` instead of `always()`, i.e. a cleanup that skips exactly the
  failed/cancelled runs that leave the most wreckage;
* a drifted command (the pre-#1538 spelling, which never covered `$RUNNER_TEMP`);
* the negative controls: a compliant self-hosted container job passes, and jobs
  that do not qualify (hosted-with-container, self-hosted-without-container) are
  not policed at all.

Run with ``python3 -m unittest discover -s tools/tests``.
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_ci_workspace_cleanup as gate  # noqa: E402

SELF_HOSTED = (
    "${{ (github.repository == 'avatarsd-llc/libtracer') && "
    "fromJSON('[\"self-hosted\",\"ci-local\"]') || 'ubuntu-24.04' }}"
)

CLEANUP = f"""      - name: {gate.STEP_NAME}
        if: {gate.STEP_IF}
        run: {gate.STEP_RUN}
"""


def workflow(runs_on: str, container: str | None, steps: str) -> str:
    """@brief Render a one-job workflow file with the given routing and steps."""
    body = f"""name: fixture
on: [push]
jobs:
  the-job:
    runs-on: {runs_on}
"""
    if container is not None:
        body += f"    container: {container}\n"
    return body + "    steps:\n" + steps


class GateArms(unittest.TestCase):
    """One synthetic workflow directory per arm; each asserts the verdict AND its reason."""

    def _findings(self, text: str) -> list[str]:
        """@brief Run the gate over a temporary workflow dir holding exactly `text`."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fixture.yml"
            path.write_text(text, encoding="utf-8")
            return [message for _, message in gate.check(Path(tmp))]

    def test_compliant_job_passes(self):
        """The positive control: without it a always-red gate would pass every arm below."""
        text = workflow(SELF_HOSTED, "ubuntu:24.04", "      - run: make\n" + CLEANUP)
        self.assertEqual(self._findings(text), [])

    def test_plain_self_hosted_list_form_is_also_policed(self):
        """`runs-on: [self-hosted, bench-local]` is the other spelling of the same pool."""
        text = workflow("[self-hosted, ci-local]", "ubuntu:24.04", "      - run: make\n")
        self.assertEqual(len(self._findings(text)), 1)

    def test_missing_step_is_red(self):
        """The 2026-08-23 defect itself: build-test-minimal-set had no cleanup at all."""
        findings = self._findings(workflow(SELF_HOSTED, "ubuntu:24.04", "      - run: make\n"))
        self.assertEqual(len(findings), 1)
        self.assertIn("does not END with", findings[0])

    def test_step_not_last_is_red(self):
        """A step after the cleanup can re-dirty the tree the cleanup just handed back."""
        text = workflow(SELF_HOSTED, "ubuntu:24.04", CLEANUP + "      - run: make artifacts\n")
        findings = self._findings(text)
        self.assertEqual(len(findings), 1)
        self.assertIn("does not END with", findings[0])

    def test_success_only_cleanup_is_red(self):
        """The failed run is the one that leaves the wreckage, so `always()` is load-bearing."""
        step = CLEANUP.replace(f"if: {gate.STEP_IF}", "if: success()")
        findings = self._findings(workflow(SELF_HOSTED, "ubuntu:24.04", step))
        self.assertEqual(len(findings), 1)
        self.assertIn("always()", findings[0])

    def test_drifted_command_is_red(self):
        """The pre-#1538 spelling: the workspace only, leaving `$RUNNER_TEMP` root-owned."""
        step = CLEANUP.replace(gate.STEP_RUN, "chown -R \"$(stat -c '%u:%g' ..)\" .")
        findings = self._findings(workflow(SELF_HOSTED, "ubuntu:24.04", step))
        self.assertEqual(len(findings), 1)
        self.assertIn("drifted", findings[0])

    def test_hosted_container_job_is_not_policed(self):
        """A hosted VM's disk is discarded with the VM; nothing survives to break."""
        text = workflow("ubuntu-24.04", "ubuntu:24.04", "      - run: make\n")
        self.assertEqual(self._findings(text), [])

    def test_self_hosted_without_container_is_not_policed(self):
        """Steps that are not in a container already run AS the runner user."""
        text = workflow("[self-hosted, bench-local]", None, "      - run: make\n")
        self.assertEqual(self._findings(text), [])


class RealWorkflows(unittest.TestCase):
    """The gate's subject really does exist — a sweep that inspects nothing is vacuous."""

    def test_the_tree_has_self_hosted_container_jobs(self):
        jobs = list(gate.qualifying_jobs())
        self.assertGreater(len(jobs), 0, "no self-hosted container job found — has the routing changed?")

    def test_the_hand_rolled_reader_agrees_with_pyyaml(self):
        """The reader is hand-rolled because CI has no PyYAML; here, where PyYAML exists, it is the oracle.

        `version-consistency.yml` runs `unittest discover -s tools/tests` with no
        pattern and no pip install, so a `import yaml` in any tool under `tools/`
        fails the whole suite — which is how this gate first went red. The parser
        that replaced it reads a subset of YAML, and a subset parser is exactly the
        thing that can be quietly wrong. On a developer machine (and nowhere else)
        this arm pins it against the real parser over the REAL workflow tree: same
        job set, same step count per job, same value for every plain scalar the gate
        reads. Skipped rather than failed where PyYAML is absent — the skip is the
        environment the gate was rewritten for.
        """
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed (the environment this parser exists for)")

        workflows = Path(REPO) / ".github" / "workflows"
        mine = {}
        for path in sorted(workflows.glob("*.yml")):
            mine[path.name] = gate.parse_workflow(path.read_text(encoding="utf-8"))

        checked_jobs = 0
        for path in sorted(workflows.glob("*.yml")):
            doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            for name, job in (doc.get("jobs") or {}).items():
                if not isinstance(job, dict):
                    continue
                self.assertIn(name, mine[path.name], f"{path.name}: job `{name}` missed by the reader")
                got = mine[path.name][name]
                self.assertEqual(
                    job.get("container") is not None, got["container"], f"{path.name}:{name} container"
                )
                # `runs-on:` is kept as RAW text by the reader (a flow sequence stays
                # `[self-hosted, bench-local]` rather than becoming a list), so the
                # comparison is the only thing the gate asks of it: the routing verdict.
                self.assertEqual(
                    "self-hosted" in str(job.get("runs-on", "")),
                    gate._is_self_hosted(got["runs-on"]),
                    f"{path.name}:{name} runs-on",
                )
                reference = job.get("steps") or []
                self.assertEqual(len(reference), len(got["steps"]), f"{path.name}:{name} step count")
                for ref, mine_step in zip(reference, got["steps"]):
                    for key in ("name", "if", "run", "uses"):
                        value = ref.get(key)
                        if isinstance(value, str) and "\n" not in value:
                            self.assertEqual(value.strip(), (mine_step.get(key) or "").strip(), f"{name}.{key}")
                checked_jobs += 1
        self.assertGreater(checked_jobs, 40, "the sweep should cover the whole workflow tree")


if __name__ == "__main__":
    unittest.main()
