#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Gate every self-hosted `container:` job on the workspace-ownership cleanup step (#1538).

WHY THIS EXISTS. A `container:` job runs as **root**, and on the self-hosted
`ci-local` runners the container's `/__w` is a bind mount of the host runner's
`_work` tree. Every file such a job creates — the checkout, the CMake build
trees, `$RUNNER_TEMP` — stays root-owned after the job ends. The runner user
(`github-runner`) then cannot clean them, so the NEXT job on that machine fails
inside `actions/checkout`'s clean step. That failure reads as code breakage and
is not: on 2026-08-23 it took `perf-local / bench-local` red with 129 root-owned
paths under `core/build-minimal`, left there by core-ci's
`build-test-minimal-set`.

The remedy is one `if: always()` step per job that hands the tree back. The
DEFECT is that the remedy is per-job: it was added to eleven of the thirteen
qualifying jobs and missed on two, and the two it missed are what broke the
bench runner. A cleanup step that a new job can silently omit is not a fix, it
is a convention. This gate turns it into a rule — a job that runs a container on
a self-hosted runner and does not end with the canonical step fails here, named.

WHY NOT A COMPOSITE ACTION (which would remove the copy-paste this gate polices):
a local `uses: ./.github/actions/...` is only resolvable after `actions/checkout`
has run, and the step must survive a job that died in its apt step BEFORE
checkout — the exact wedge surface core-ci's header documents. The duplicated
inline step is deliberate; this gate is what keeps the duplicates identical.

WHAT COUNTS AS QUALIFYING. A job whose `runs-on:` mentions `self-hosted` (either
literally or inside the trusted-run routing expression) AND which declares a
`container:`. Hosted-only jobs need nothing — the whole VM is discarded — and
non-container self-hosted jobs already run as `github-runner`.

WHAT IS ENFORCED. The job's LAST step must be exactly the canonical step: the
canonical name, `if: always()`, and the canonical `run:` command, character for
character. Last, because a step after it can re-dirty the tree it just handed
back. Identical, because a variant that chowns a smaller set is the same defect
wearing the same name.

STDLIB ONLY. `version-consistency.yml` runs `unittest discover -s tools/tests` with
no pattern and no pip install, so an `import yaml` here fails a suite that has
nothing to do with this gate — it did, on the first cut. `parse_workflow` below
reads the small YAML subset the gate needs instead, and its unit tests pin it
against the real parser over the real workflow tree wherever PyYAML is installed.

Usage:
    python3 tools/check_ci_workspace_cleanup.py            # gate (exit 1 on drift)
    python3 tools/check_ci_workspace_cleanup.py --list     # report every job it inspects
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

WORKFLOW_DIR = Path(__file__).resolve().parent.parent / ".github" / "workflows"

STEP_NAME = "Restore workspace ownership for the host runner"
"""The canonical step name. Also what the failure message tells an author to add."""

STEP_RUN = 'chown -R "$(stat -c \'%u:%g\' ..)" . "$RUNNER_TEMP"'
"""The canonical command.

`..` is the workspace's parent (`/__w/<repo>`), created by the runner process on
the host, so its owner IS the runner user — no uid is hard-coded and the job
behaves identically on the hosted fallback route. `.` is the workspace itself and
`$RUNNER_TEMP` is `/__w/_temp`, which `install-consume` builds into as root and
which the runner likewise cannot clear between jobs.
"""

STEP_IF = "always()"
"""Success is not the interesting case. A job that failed or was cancelled mid-build
leaves the most root-owned wreckage, and that is the run whose next checkout breaks."""

CANONICAL_YAML = f"""      - name: {STEP_NAME}
        if: {STEP_IF}
        run: {STEP_RUN}"""


def _strip_inline_comment(text: str) -> str:
    """@brief Drop a trailing `# …` comment, respecting quotes.

    `name: "Allocation escape gate (guard) — #848"` is a real step in core-ci.yml,
    and a naive `split('#')` truncates it into a different step name.
    """
    quote = ""
    for i, ch in enumerate(text):
        if quote:
            if ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch == "#" and (i == 0 or text[i - 1] in " \t"):
            return text[:i]
    return text


def _scalar(raw: str) -> str:
    """@brief Unquote a plain/single/double-quoted YAML scalar. No escapes are needed here."""
    value = _strip_inline_comment(raw).strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def parse_workflow(text: str) -> dict:
    """@brief Read the `jobs:` block of a workflow into the shape this gate needs.

    A HAND-ROLLED reader rather than PyYAML, on purpose: `version-consistency.yml`
    runs `unittest discover -s tools/tests` with no pattern and no pip install, so
    every tool under `tools/` has to import on a bare python. The subset parsed here
    is exactly what the gate asks: per job, the raw `runs-on:` text, whether a
    `container:` key exists, and each step's top-level scalar keys.

    Anything it cannot read as a plain scalar (a block scalar `|`, a nested mapping
    like `with:`) is recorded as the sentinel `None` rather than guessed at. For the
    canonical cleanup step — three plain scalars — that loses nothing, and a job that
    writes it some other way is drift the gate SHOULD name.
    """
    jobs: dict[str, dict] = {}
    lines = text.split("\n")
    i, in_jobs = 0, False
    job: dict | None = None
    step: dict | None = None
    step_key_indent = -1
    steps_seq_indent = -1
    in_steps = False

    while i < len(lines):
        line = lines[i]
        i += 1
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        body = line.strip()

        if indent == 0:
            in_jobs = body == "jobs:"
            job = step = None
            continue
        if not in_jobs:
            continue

        if indent == 2 and body.endswith(":"):
            job = {"runs-on": "", "container": False, "steps": []}
            jobs[body[:-1].strip()] = job
            step, in_steps, steps_seq_indent = None, False, -1
            continue
        if job is None:
            continue

        if indent == 4:
            step = None
            key, _, rest = body.partition(":")
            # `in_steps` is what keeps a `strategy: matrix: include:` sequence — whose
            # `- acl_full: OFF` entries look exactly like steps — out of the step list.
            in_steps = key == "steps"
            if key == "runs-on":
                job["runs-on"] = _strip_inline_comment(rest).strip()
            elif key == "container":
                job["container"] = True
            continue
        if not in_steps:
            continue

        # A step opens with `- ` at the steps-sequence indent; its remaining keys sit
        # one level in. Both are recorded, so `- name: x` followed by `if:`/`run:` reads
        # as one mapping.
        if body.startswith("- ") and steps_seq_indent in (-1, indent):
            steps_seq_indent = indent
            step = {}
            job["steps"].append(step)
            step_key_indent = indent + 2
            body = body[2:]
        elif step is None or indent != step_key_indent:
            continue

        key, sep, rest = body.partition(":")
        if not sep or " " in key:
            continue
        rest = rest.strip()
        if rest.startswith(("|", ">")) or rest == "":
            # A block scalar or a nested mapping: consume its body, record the sentinel.
            step[key] = None
            while i < len(lines) and (not lines[i].strip() or len(lines[i]) - len(lines[i].lstrip()) > indent):
                i += 1
            continue
        step[key] = _scalar(rest)

    return jobs


def _is_self_hosted(runs_on: object) -> bool:
    """@brief True when `runs-on:` can resolve to the self-hosted pool.

    Covers the plain list form (`[self-hosted, bench-local]`) and the trusted-run
    routing expression, which names the pool inside a `fromJSON('[...]')` literal.
    Matching the raw text is deliberate: the expression cannot be evaluated here,
    and a job that MIGHT land on the hardware has to carry the cleanup.
    """
    return "self-hosted" in str(runs_on)


def qualifying_jobs(workflow_dir: Path = WORKFLOW_DIR):
    """@brief Yield `(path, job_name, job)` for every self-hosted containerized job."""
    for path in sorted(workflow_dir.glob("*.yml")):
        for name, job in parse_workflow(path.read_text(encoding="utf-8")).items():
            if not job["container"]:
                continue
            if not _is_self_hosted(job["runs-on"]):
                continue
            yield path, name, job


def check(workflow_dir: Path = WORKFLOW_DIR) -> list[tuple[str, str]]:
    """@brief Return `(job_key, finding)` per offending job (empty list == green)."""
    findings: list[tuple[str, str]] = []
    for path, name, job in qualifying_jobs(workflow_dir):
        rel = path.relative_to(workflow_dir.parent.parent)
        key = f"{path.name}:{name}"

        def note(text: str, _key: str = key) -> None:
            """@brief Record one finding against the job currently being inspected."""
            findings.append((_key, text))

        steps = job.get("steps") or []
        if not steps:
            note(f"{rel}: job `{name}` runs a container on a self-hosted runner but has no steps")
            continue
        last = steps[-1]
        if not isinstance(last, dict) or last.get("name") != STEP_NAME:
            shown = last.get("name") or last.get("uses") or "<unnamed>" if isinstance(last, dict) else str(last)
            note(
                f"{rel}: job `{name}` does not END with the workspace-ownership cleanup step "
                f"(last step is {shown!r})"
            )
            continue
        if (last.get("if") or "").strip() != STEP_IF:
            note(f"{rel}: job `{name}` cleanup step is not `if: {STEP_IF}` (a green-only cleanup is not one)")
        if (last.get("run") or "").strip() != STEP_RUN:
            note(
                f"{rel}: job `{name}` cleanup step command drifted\n"
                f"       expected: {STEP_RUN}\n"
                f"       found:    {(last.get('run') or '<not a plain scalar>').strip()}"
            )
    return findings


def main() -> int:
    """@brief CLI entry point: `--list` reports, the default gates."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true", help="print every inspected job and its verdict")
    args = parser.parse_args()

    jobs = list(qualifying_jobs())
    findings = check()
    offenders = {key for key, _ in findings}

    if args.list:
        for path, name, _ in jobs:
            key = f"{path.name}:{name}"
            print(f"{'FAIL' if key in offenders else 'ok  '}  {key}")

    if findings:
        print(
            f"{len(offenders)} self-hosted container job(s) do not restore workspace ownership (#1538):\n",
            file=sys.stderr,
        )
        for _, finding in findings:
            print(f"  - {finding}", file=sys.stderr)
        print(
            "\nEvery job that runs `container:` on a self-hosted runner must END with:\n\n"
            f"{CANONICAL_YAML}\n\n"
            "Without it the container's root-owned files survive the job and the next\n"
            "actions/checkout on that runner fails its clean step.",
            file=sys.stderr,
        )
        return 1

    print(f"ok: all {len(jobs)} self-hosted container jobs restore workspace ownership")
    return 0


if __name__ == "__main__":
    sys.exit(main())
