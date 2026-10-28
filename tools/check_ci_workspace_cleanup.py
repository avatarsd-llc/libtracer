#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Gate every self-hosted job on the workspace check and cleanup steps (#1538, #2343).

WHY THIS EXISTS. The self-hosted runners share one `_work` tree on a tmpfs, so
what a job leaves there is both a hazard and a cost. Two incidents, one per step:

- #1538: a `container:` job runs as **root**, and the container's `/__w` is a
  bind mount of the host runner's `_work` tree. Every file it creates stays
  root-owned, the runner user cannot clean it, and the NEXT job on that machine
  fails inside `actions/checkout`'s clean step. On 2026-08-23 that took
  `perf-local / bench-local` red with 129 root-owned paths left by core-ci's
  `build-test-minimal-set`, one of two jobs the per-job remedy had missed.
- #2343: build trees from finished jobs piled up on the runner tmpfs until a job
  could not start, and the failure read as a test failure.

So every self-hosted job is bracketed by two canonical steps. FIRST,
`Check the runner workspace has room`, which fails below 4 GB free with a message
that names the runner, not the code. LAST, under `if: always()`, the cleanup:
it deletes the job's checkout, build trees, workspace siblings and temp files, and
hands the runner's own `$RUNNER_TEMP/_*` dirs back to the runner user. It runs as
root in a container, and a host job first hands root-owned leftovers back through a
throwaway container (no sudo), so root-owned files go too. A remedy that a new job
can silently omit is a convention, not a fix; this gate makes it a rule, naming each
job that lacks either step.

WHY NOT A COMPOSITE ACTION (which would remove the copy-paste this gate polices):
a local `uses: ./.github/actions/...` is only resolvable after `actions/checkout`
has run. The check runs before checkout, and the cleanup must survive a job that
died before checkout. The duplicated inline steps are deliberate; this gate is
what keeps the duplicates identical.

WHAT COUNTS AS QUALIFYING. A job whose `runs-on:` mentions `self-hosted` (either
literally or inside the trusted-run routing expression). Hosted-only jobs need
nothing; the whole VM is discarded.

WHAT IS ENFORCED. Three things.

1. The job's FIRST step is exactly the canonical space check, so a full disk is
   named before checkout can fail on it.
2. The job's LAST step is exactly the canonical cleanup: the canonical name,
   `if: always()`, and the canonical `run:` command, character for character.
   Last, because a step after it can re-dirty the tree. Identical, because a
   variant that cleans a smaller set is the same defect wearing the same name.
3. In a `container:` job, `actions/checkout` sets `persist-credentials: false`.
   Checkout's POST step runs after every real step, including the cleanup, and
   rewrites `.git/config` as root when it has a credential to unset; see
   CHECKOUT_WITH. (With the cleanup deleting `.git` this is belt and braces: the
   post step skips when `.git/config` is gone. It still matters for a job whose
   cleanup could not finish.)

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

SPACE_NAME = "Check the runner workspace has room"
"""The canonical first step's name."""

SPACE_MIN_KIB = 4 * 1024 * 1024
"""The threshold, 4 GB in the KiB that `df -Pk` reports."""

SPACE_RUN = (
    "a=$(df -Pk \"$GITHUB_WORKSPACE\" | awk 'NR==2 {print $4}'); "
    f'[ "${{a:-0}}" -ge {SPACE_MIN_KIB} ] || '
    '{ echo "::error::runner workspace has $(( ${a:-0} / 1048576 )) GB free (< 4 GB); '
    'clean the runner, this is not a test failure"; exit 1; }'
)
"""The canonical space check.

It runs BEFORE checkout, so it can only use the shell. `df -P` keeps the output
on one line per filesystem whatever the device name's length. An unreadable `df`
reads as 0 and fails, which is the right answer for a workspace that cannot be
inspected. It cannot catch a disk too full for the runner to download the job's
actions: that dies in "Set up job" before any step, and is what the cleanup
below prevents.
"""

STEP_NAME = "Clean this job's workspace off the host runner"
"""The canonical last step's name. Also what the failure message tells an author to add."""

STEP_RUN = (
    'w="${GITHUB_WORKSPACE%/}"; w="${w:?}"; t="${RUNNER_TEMP:?}"; r=0; '
    'if [ "$(id -u)" != 0 ] && [ "$RUNNER_ENVIRONMENT" = self-hosted ]; then '
    'docker run --rm -v "$w/..:/w" -v "$t:/t" ubuntu:24.04 chown -R "$(id -u):$(id -g)" /w /t || r=1; fi; '
    'find "$w/.." -mindepth 1 -maxdepth 1 ! -path "$w/../${w##*/}" -exec rm -rf {} + || r=1; '
    'find "$w" -mindepth 1 -maxdepth 1 -exec rm -rf {} + || r=1; '
    "find \"$t\" -mindepth 1 -maxdepth 1 ! -name '_*' -exec rm -rf {} + || r=1; "
    "chown -R \"$(stat -c '%u:%g' \"$w/..\")\" \"$t\" || r=1; "
    '[ "$r" = 0 ] || echo "::warning::this job left files in the runner workspace; '
    "the next job's free-space check names a full disk\""
)
"""The canonical cleanup command.

No `sudo` (#2348): the runner user holds no sudo grant. Inside a container the
step is already root. On a host self-hosted job, which runs as the runner user,
a throwaway `ubuntu:24.04` container first hands every file under the workspace's
parent and `$RUNNER_TEMP` back to that user, so the root-owned leftovers of a
container job that died before its own cleanup can be deleted. On the hosted
fallback there is nothing root-owned to remove, so that container is skipped. The `:?` guards make an unset variable abort the step
instead of widening `find` to `/`, and the trailing `/` is stripped first so
`${w##*/}` always names the workspace dir. Each part runs whatever the one
before it did (`;`, not `&&`): a delete that fails must not skip the `_temp`
chown, or a root-owned `_runner_file_commands` breaks the next host job's
"Set up job" (#1538).

- The workspace's siblings go (perf's `../main-baseline` worktree, perf-local's
  `../ghp-prune`), but not the workspace dir itself: post steps run in it, and
  `actions/checkout`'s post step skips once `.git/config` is gone.
- The workspace's contents go: the checkout and every build tree.
- `$RUNNER_TEMP` keeps the runner's own `_*` dirs (it reads its file-command
  files after this step) and loses the job's files, e.g. `install-consume`'s
  build. What is left is handed to the owner of `..`, the workspace's parent,
  which the runner process created on the host, so its owner IS the runner user
  and no uid is hard-coded.

A failure warns rather than fails: the job's verdict is about the code, and the
next job's space check is the alarm for a runner that is filling up.
"""

STEP_IF = "always()"
"""Success is not the interesting case. A job that failed or was cancelled mid-build
leaves the most wreckage, and that is the run whose next job breaks."""

CANONICAL_YAML = f"""      - name: {SPACE_NAME}
        run: {SPACE_RUN}
      ...
      - name: {STEP_NAME}
        if: {STEP_IF}
        run: {STEP_RUN}"""

CHECKOUT_WITH = ("persist-credentials", "false")
"""The one setting that keeps the cleanup step from being undone a second later.

`actions/checkout`'s POST step calls `removeAuth()` UNCONDITIONALLY, and
`git config --local --unset-all http.<url>.extraheader` REWRITES `.git/config`
when it matches — as root, after the last real step has run. Measured on this
host: `--unset-all` with a match re-owns the file to the caller; with no match it
exits 5 and does not touch it. So a container job that persists credentials leaves
exactly one root-owned file behind no matter where the cleanup step sits, and that
one file is `.git/config` — precisely what the next checkout must write to clean.
It was observed after the FIRST green run of the fixed workflows: 129 root-owned
paths down to 1, and the 1 was this.

`persist-credentials: false` moves the removal into the MAIN step (root, before the
cleanup) and leaves the post step nothing to match. None of these jobs use git
credentials after checkout — they build, test and lint — so nothing else changes.
"""


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
            # A block scalar or a nested mapping. Its body is consumed either way; a
            # nested mapping of plain scalars (`with:`) is KEPT, because the gate reads
            # `persist-credentials` out of the checkout step's. Anything else records
            # the sentinel `None` rather than a guess.
            nested: dict[str, str] | None = {} if key == "with" else None
            while i < len(lines) and (not lines[i].strip() or len(lines[i]) - len(lines[i].lstrip()) > indent):
                child = lines[i]
                i += 1
                inner = child.strip()
                if not inner or inner.startswith("#") or nested is None:
                    continue
                child_key, child_sep, child_rest = inner.partition(":")
                if not child_sep or " " in child_key or child_rest.strip() == "":
                    nested = None
                    continue
                nested[child_key] = _scalar(child_rest)
            step[key] = nested if nested else None
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
    """@brief Yield `(path, job_name, job)` for every job that can run self-hosted."""
    for path in sorted(workflow_dir.glob("*.yml")):
        for name, job in parse_workflow(path.read_text(encoding="utf-8")).items():
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
            note(f"{rel}: job `{name}` runs on a self-hosted runner but has no steps")
            continue

        # The checkout POST step runs AFTER the cleanup and re-roots `.git/config`
        # unless there is no persisted credential left for it to unset. See
        # CHECKOUT_WITH — this is not tidiness, it is the difference between "no
        # root-owned files" and "one, and it is the one that breaks the next job".
        setting, expected = CHECKOUT_WITH
        for step in steps if job["container"] else []:
            uses = step.get("uses") or ""
            if not uses.startswith("actions/checkout"):
                continue
            with_block = step.get("with")
            got = with_block.get(setting) if isinstance(with_block, dict) else None
            if (got or "").strip().lower() != expected:
                note(
                    f"{rel}: job `{name}` checks out with `{setting}` = {got or '<unset>'}; "
                    f"it must be `{expected}`, or checkout's post step re-roots `.git/config` "
                    f"after the cleanup has run"
                )

        first = steps[0]
        if not isinstance(first, dict) or first.get("name") != SPACE_NAME:
            shown = first.get("name") or first.get("uses") or "<unnamed>" if isinstance(first, dict) else str(first)
            note(
                f"{rel}: job `{name}` does not START with the runner workspace space check "
                f"(first step is {shown!r})"
            )
        elif (first.get("run") or "").strip() != SPACE_RUN:
            note(
                f"{rel}: job `{name}` space check command drifted\n"
                f"       expected: {SPACE_RUN}\n"
                f"       found:    {(first.get('run') or '<not a plain scalar>').strip()}"
            )

        last = steps[-1]
        if not isinstance(last, dict) or last.get("name") != STEP_NAME:
            shown = last.get("name") or last.get("uses") or "<unnamed>" if isinstance(last, dict) else str(last)
            note(
                f"{rel}: job `{name}` does not END with the workspace cleanup step "
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
            f"{len(offenders)} self-hosted job(s) lack the workspace check or cleanup (#1538, #2343):\n",
            file=sys.stderr,
        )
        for _, finding in findings:
            print(f"  - {finding}", file=sys.stderr)
        print(
            "\nEvery job that can run on a self-hosted runner must START and END with:\n\n"
            f"{CANONICAL_YAML}\n\n"
            "Without them a finished job's files (root-owned, from a container) fill the\n"
            "runner tmpfs and break the next checkout, and the breakage reads as a test failure.",
            file=sys.stderr,
        )
        return 1

    print(f"ok: all {len(jobs)} self-hosted jobs check and clean the runner workspace")
    return 0


if __name__ == "__main__":
    sys.exit(main())
