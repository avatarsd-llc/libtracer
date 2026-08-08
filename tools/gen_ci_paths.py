#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Derive core-ci.yml's ``paths:`` trigger from what core-ci actually builds.

``core-ci`` is the one workflow that owns the whole host ``ctest`` suite, and its
trigger was a HAND-WRITTEN allowlist: ``core/**``, ``bench/**``,
``tests/packaging/**``. Several ctest targets compile or read files that live
OUTSIDE those roots -- the ESP-IDF host suites name
``integrations/esp-idf/libtracer/*.cpp`` directly as target sources, and a group of
targets is handed ``tests/conformance/vectors/v1`` as a compile definition. Neither
tree was in the list, so a pull request confined to one of them ran NO host test at
all, from the very suites written to cover it (#1082).

Adding the two missing entries by hand would repair today and rot at the next suite
that reaches outside ``core/``. So the list is DERIVED instead:

  * every ``cmake -S <dir>`` the workflow runs contributes ``<dir>/**`` -- those are
    the project roots its jobs configure (the core, the bench tree, the packaging
    fixture);
  * the core project is configured with ``-DBUILD_TESTING=ON`` and its CMake
    file-api codemodel is read back. Every target source, include directory,
    path-valued compile definition and ctest property that resolves inside the
    repository contributes a pattern of its own: a directory as ``<dir>/**``, a file
    as its exact path;
  * a pattern that a broader pattern already covers is dropped, so the emitted list
    is the minimal one that covers every input.

The workflow file itself is appended -- it is an input to its own runs and no
configure can report it.

Why this cannot silently rot: making a ctest target compile or read a new tree means
editing a ``CMakeLists.txt`` under ``core/``, and ``core/**`` is a root, so core-ci
fires and this check runs. The derived list can go stale only in a run that is
already gated on it.

Scope: this derives ONE workflow's trigger, core-ci.yml. The four other workflows
that build or run ctest were audited for the same hole while fixing #1082; the
result is recorded here so the audit is not repeated. None of them is the gate for
the host suites, and none is changed by this script.

  * ``docs.yml`` -- runs a ctest sweep (``bench/gen_test_report.py``) and its trigger
    names neither ``core/src/**`` nor ``integrations/**``, so the sweep is skipped for
    a change to either. NOT a gate hole: ``gen_test_report.py`` calls ``ctest``
    through ``subprocess.run`` with no ``check=`` and discards the exit code -- it
    renders the outcome into ``docs/test-report.md``. A failing suite there is a red
    row on a published page, not a red check. Residual, unfixed: the published report
    can describe a tree the sweep never re-ran on.
  * ``capability-matrix.yml`` -- NEGATIVE, and the premise is wrong:
    ``tools/gen_capability_matrix.py`` imports no ``subprocess`` and invokes no
    ctest. It cites ctest NAMES as evidence and checks the named artifacts exist.
    Its ``pull_request`` filter already carries ``integrations/esp-idf/**`` and
    ``tests/conformance/vectors/**``.
  * ``can-vcan-e2e.yml`` -- builds every core target (so it does compile the ESP-IDF
    host suites) but RUNS one test, ``-R '^transport_can_vcan$'``. It is not the gate
    for the host suites and adding ``integrations/**`` to it would buy nothing that
    core-ci does not now buy. Its own filter is narrow in the other direction -- four
    named ``core/`` files while the test links all of ``libtracer`` -- which is the
    ``quic.yml`` defect, not this one.
  * ``quic.yml`` -- runs the FULL ctest with ``LIBTRACER_WITH_QUIC=ON``, so it does
    compile the host suites, and its ``pull_request`` allowlist names individual
    ``core/`` files with no ``integrations/**``. Same SHAPE, but not a coverage hole
    for those suites: core-ci is their gate and quic.yml's run of them is incidental.
    Its known residual -- an allowlist naming specific ``.cpp`` files while the QUIC
    TUs also consume shared core headers -- is pre-existing and out of scope here.

Usage::

    python3 tools/gen_ci_paths.py --check   # exit 1 if the committed list drifted
    python3 tools/gen_ci_paths.py --apply   # rewrite the paths: blocks in place
    python3 tools/gen_ci_paths.py           # print the derived list, change nothing

Requires ``cmake`` and a working C++ compiler: the derivation is a real configure of
the core project, not a parse of the CMake text.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "core-ci.yml"
WORKFLOW_REL = ".github/workflows/core-ci.yml"

# The core project is the one whose test targets reach outside their own root, so it
# is the one that gets configured. `-DBUILD_TESTING=ON` is what registers the tests;
# core-ci's other configure options (LIBTRACER_ACL_FULL, LIBTRACER_LKV_SLOT) only set
# compile definitions inside core/CMakeLists.txt and add no sources, so they cannot
# change the derived set.
CORE_PROJECT = "core"
CONFIGURE_ARGS = ["-DBUILD_TESTING=ON"]

# Every generated note line carries MARKER, which is how `--apply` recognises its own
# previous output and replaces it instead of stacking a second copy above the key.
MARKER = "gen_ci_paths.py"
GENERATED_NOTE = [
    "# DERIVED by tools/gen_ci_paths.py -- do not hand-edit: this list is what",
    "# core-ci's jobs configure and compile. `gen_ci_paths.py --apply` rewrites it.",
]


# --- workflow parsing -------------------------------------------------------


def source_roots(text: str) -> set[str]:
    """@brief Repo-relative project roots the workflow configures via ``cmake -S``."""
    roots = set()
    for raw in re.findall(r"cmake\s+-S\s+(\S+)", text):
        cand = raw.strip("\"'")
        if "$" in cand:  # a variable-built path is not resolvable here
            continue
        p = (ROOT / cand).resolve()
        if p.is_dir() and p != ROOT and ROOT in p.parents:
            roots.add(p.relative_to(ROOT).as_posix())
    return roots


def _clean_item(value: str) -> str:
    """@brief Strip a trailing YAML comment and surrounding quotes from a list item."""
    value = value.strip()
    if value and value[0] in "\"'":
        quote = value[0]
        end = value.find(quote, 1)
        if end != -1:
            return value[1:end]
    return value.split("#", 1)[0].strip()


def _split_flow(rest: str) -> list[str]:
    """@brief Items of an inline ``paths: ["a", "b"]`` flow sequence."""
    inner = rest.strip()[1:-1] if rest.strip().endswith("]") else rest.strip()[1:]
    return [_clean_item(part) for part in inner.split(",") if _clean_item(part)]


def paths_blocks(lines: list[str]) -> list[tuple[int, int, str, list[str]]]:
    """@brief Locate every ``paths:`` list under ``on:``.

    Returns ``(start, end, indent, patterns)`` per block, where ``[start, end)`` is
    the line span to replace -- generated comment lines immediately above the
    ``paths:`` key are folded into the span so repeated ``--apply`` runs do not stack
    them up.
    """
    out: list[tuple[int, int, str, list[str]]] = []
    in_on = False
    for i, line in enumerate(lines):
        if not line[:1].isspace() and line.strip():
            in_on = line.startswith("on:")
            continue
        if not in_on:
            continue
        m = re.match(r"^(?P<indent>\s*)paths:(?P<rest>.*)$", line)
        if not m:
            continue
        indent, rest = m.group("indent"), m.group("rest").strip()
        start = i
        while start > 0 and lines[start - 1].lstrip().startswith("#") \
                and MARKER in lines[start - 1]:
            start -= 1
        if rest.startswith("["):
            out.append((start, i + 1, indent, _split_flow(rest)))
            continue
        j, items = i + 1, []
        while j < len(lines):
            item = re.match(r"^(?P<pad>\s*)-\s*(?P<val>.+?)\s*$", lines[j])
            if not item or len(item.group("pad")) <= len(indent):
                break
            items.append(_clean_item(item.group("val")))
            j += 1
        out.append((start, j, indent, items))
    return out


def render(indent: str, patterns: list[str]) -> list[str]:
    """@brief The generated replacement lines for one ``paths:`` block."""
    lines = [indent + note for note in GENERATED_NOTE]
    lines.append(indent + "paths:")
    lines.extend(f'{indent}  - "{p}"' for p in patterns)
    return lines


# --- derivation -------------------------------------------------------------


def configure(build_dir: pathlib.Path) -> None:
    """@brief Configure the core project with a CMake file-api codemodel query."""
    query = build_dir / ".cmake" / "api" / "v1" / "query"
    query.mkdir(parents=True, exist_ok=True)
    (query / "codemodel-v2").touch()
    cmd = ["cmake", "-S", str(ROOT / CORE_PROJECT), "-B", str(build_dir), *CONFIGURE_ARGS]
    try:
        done = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        raise SystemExit("error: cmake not found — this check derives the trigger from "
                         "a real configure, so it needs cmake and a C++ compiler")
    if done.returncode != 0:
        sys.stderr.write(done.stdout + done.stderr)
        raise SystemExit(f"error: configure failed: {' '.join(cmd)}")


def _absolute_paths_in(text: str) -> list[str]:
    """@brief Absolute path-looking substrings of a compile definition or property."""
    return re.findall(r"/(?:[\w.+-]+/)*[\w.+-]+", text)


def referenced_paths(build_dir: pathlib.Path) -> set[pathlib.Path]:
    """@brief Every in-repo file or directory the configured core project consumes."""
    reply = build_dir / ".cmake" / "api" / "v1" / "reply"
    index = sorted(reply.glob("index-*.json"))
    if not index:
        raise SystemExit(f"error: no file-api reply under {reply}")
    objects = json.loads(index[-1].read_text("utf-8"))["objects"]
    codemodel = next(o for o in objects if o["kind"] == "codemodel")
    model = json.loads((reply / codemodel["jsonFile"]).read_text("utf-8"))
    source_dir = pathlib.Path(model["paths"]["source"])

    raw: list[str] = []
    for config in model["configurations"]:
        for entry in config["targets"]:
            target = json.loads((reply / entry["jsonFile"]).read_text("utf-8"))
            raw.extend(s["path"] for s in target.get("sources", []))
            for group in target.get("compileGroups", []):
                raw.extend(inc["path"] for inc in group.get("includes", []))
                for define in group.get("defines", []):
                    raw.extend(_absolute_paths_in(define["define"]))

    # ctest properties (WORKING_DIRECTORY, ENVIRONMENT, ...) can name a data file no
    # compile flag mentions. CTestTestfile.cmake exists after configure, so this needs
    # no build. Contributes nothing today; it is here so a future data-driven test
    # cannot be the reference that escapes the derivation.
    try:
        shown = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
            capture_output=True, text=True)
    except FileNotFoundError:
        raise SystemExit("error: ctest not found — it ships with cmake; check the install")
    if shown.returncode == 0:
        for test in json.loads(shown.stdout).get("tests", []):
            raw.extend(str(arg) for arg in test.get("command", []))
            for prop in test.get("properties", []):
                raw.extend(_absolute_paths_in(str(prop.get("value", ""))))

    found: set[pathlib.Path] = set()
    for item in raw:
        p = pathlib.Path(item)
        if not p.is_absolute():
            p = source_dir / p
        try:
            p = p.resolve()
        except OSError:
            continue
        # A reference that resolves to nothing on disk is not an input a `paths:` filter
        # could ever match, so it is dropped rather than turned into a dead pattern.
        if ROOT not in p.parents or not p.exists():
            continue
        if build_dir.resolve() == p or build_dir.resolve() in p.parents:
            continue  # generated headers, test binaries: build output, not an input
        found.add(p.relative_to(ROOT))
    return found


def minimal_patterns(roots: set[str], refs: set[pathlib.Path]) -> list[str]:
    """@brief Collapse roots and references to the smallest covering pattern list.

    A directory becomes ``<dir>/**``; a file stays exact. A pattern already covered
    by another is dropped, so a source file inside an include directory that is
    itself referenced adds nothing.
    """
    dirs = {pathlib.PurePosixPath(r) for r in roots}
    files: set[pathlib.PurePosixPath] = set()
    for rel in refs:
        posix = pathlib.PurePosixPath(rel.as_posix())
        (dirs if (ROOT / rel).is_dir() else files).add(posix)

    kept_dirs = {d for d in dirs if not any(other in d.parents for other in dirs)}
    kept_files = {f for f in files if not any(d in f.parents for d in kept_dirs)}
    return sorted([f"{d}/**" for d in kept_dirs] + [str(f) for f in kept_files])


def derive() -> list[str]:
    """@brief The full derived ``paths:`` list, workflow self-reference last."""
    text = WORKFLOW.read_text("utf-8")
    with tempfile.TemporaryDirectory(prefix="libtracer-ci-paths-") as tmp:
        build_dir = pathlib.Path(tmp) / "build"
        configure(build_dir)
        refs = referenced_paths(build_dir)
    patterns = minimal_patterns(source_roots(text), refs)
    return [p for p in patterns if p != WORKFLOW_REL] + [WORKFLOW_REL]


# --- entry point ------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="fail if the committed paths: list is not the derived one")
    mode.add_argument("--apply", action="store_true",
                      help="rewrite the paths: blocks with the derived list")
    args = ap.parse_args()

    if not WORKFLOW.is_file():
        print(f"error: {WORKFLOW_REL} not found", file=sys.stderr)
        return 2

    expected = derive()
    lines = WORKFLOW.read_text("utf-8").splitlines()
    blocks = paths_blocks(lines)
    if not blocks:
        print(f"error: no paths: list found under `on:` in {WORKFLOW_REL} — the "
              "trigger shape changed; update tools/gen_ci_paths.py", file=sys.stderr)
        return 2

    if args.apply:
        for start, end, indent, _ in reversed(blocks):
            lines[start:end] = render(indent, expected)
        WORKFLOW.write_text("\n".join(lines) + "\n", "utf-8")
        print(f"applied: {len(blocks)} paths: block(s) in {WORKFLOW_REL} "
              f"set to {len(expected)} derived pattern(s).")
        return 0

    if not args.check:
        for pattern in expected:
            print(pattern)
        return 0

    ok = True
    for _, _, _, committed in blocks:
        missing = [p for p in expected if p not in committed]
        extra = [p for p in committed if p not in expected]
        if not missing and not extra:
            continue
        ok = False
        for p in missing:
            print(f"ERROR: {WORKFLOW_REL} paths: is MISSING {p!r} — core-ci does not "
                  "run for a pull request confined to it, yet its jobs build from it.",
                  file=sys.stderr)
        for p in extra:
            print(f"ERROR: {WORKFLOW_REL} paths: carries {p!r}, which nothing core-ci "
                  "builds refers to (or a broader pattern already covers it).",
                  file=sys.stderr)
    if not ok:
        print("\nRegenerate with: python3 tools/gen_ci_paths.py --apply",
              file=sys.stderr)
        return 1

    print(f"ok: {WORKFLOW_REL} triggers on all {len(expected)} derived path(s) "
          f"({len(blocks)} block(s) checked).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
