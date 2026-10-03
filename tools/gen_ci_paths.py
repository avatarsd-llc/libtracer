#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Derive the path gates of core-ci.yml and docs.yml from what their jobs actually read.

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

Where the list lives (#1614). It was core-ci's ``on: paths:`` trigger filter. That
made the required check ``build-test`` path-filtered: a diff outside the list (any
docs-only pull request) never queued core-ci at all, so the check never REPORTED
and the pull request stayed blocked on a result that could not arrive. core-ci now
triggers on every change; its ``changes`` job matches the diff against this list,
held as the ``CORE_CI_PATHS: |`` block scalar in that job's ``env:``, and every
build job runs only when something matched. The ``build-test`` aggregator then
reports on every pull request: green when the jobs ran and passed, green when they
were legitimately skipped, red otherwise. Because both halves -- "run" and "skip" --
come from this ONE derived list, they are exhaustive and disjoint by construction;
there is no hand-written complement to drift.

Why this cannot silently rot: making a ctest target compile or read a new tree means
editing a ``CMakeLists.txt`` under ``core/``, and ``core/**`` is a root, so
core-ci's jobs run and this check runs with them. The derived list can go stale
only in a run that is already gated on it.

docs.yml (#1614). It owns the other path-dependent required check, ``build``, and
gets the same shape: an always-triggered workflow, a ``changes`` job reading the
``DOCS_PATHS`` block, and an aggregator named ``build``. Its list is derived from:

  * every ``cmake -S <dir>`` it runs, plus the core configure's references above
    (it configures the core with ``-DBUILD_TESTING=ON`` for the test report);
  * the Sphinx config directory (``sphinx-build -c``) and every source that
    ``docs/conf.py``'s literal ``include_patterns`` admits;
  * every file a published page pulls in by ``{literalinclude}`` or ``{include}``
    (the binding examples live outside ``docs/``);
  * the directory of every script a ``run:`` step executes, and the Doxyfile;
  * ``DOCS_DECLARED``: inputs that only script code names, each with its reason.

This also closes the old residual that docs.yml's hand-written trigger named
neither ``core/src/**`` nor ``integrations/**``, so its test-report sweep could
describe a tree it never re-ran on.

Scope beyond those two. The other workflows that build or run ctest were audited
for the same hole while fixing #1082; the result is recorded here so the audit is
not repeated. None of them is the gate for the host suites or owns a required
check, and none is changed by this script.

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
    python3 tools/gen_ci_paths.py --apply   # rewrite the CORE_CI_PATHS block in place
    python3 tools/gen_ci_paths.py           # print the derived list, change nothing

Requires ``cmake`` and a working C++ compiler: the derivation is a real configure of
the core project, not a parse of the CMake text.
"""
from __future__ import annotations

import argparse
import ast
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORKFLOWS_REL = ".github/workflows"

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


def generated_note(workflow: str) -> list[str]:
    """@brief The comment lines written above a generated block."""
    return [
        "# DERIVED by tools/gen_ci_paths.py -- do not hand-edit: this list is what",
        f"# {workflow}'s jobs read and build. `gen_ci_paths.py --apply` rewrites it.",
    ]


# Inputs of the docs build that only SCRIPT CODE names, so no configure, Sphinx config
# or workflow command can report them. Each is declared here with its reason and is
# part of the checked list like any derived entry; keep this list short.
DOCS_DECLARED = {
    # bench/gen_results_page.py runs tests/conformance/run-all.py for the published
    # conformance matrix; the harness and its vectors live under this tree.
    "tests/conformance": "conformance harness run by bench/gen_results_page.py",
}


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


def pattern_blocks(lines: list[str], key: str) -> list[tuple[int, int, str, list[str]]]:
    """@brief Locate every ``<key>: |`` block scalar in a workflow.

    The list is not a trigger filter (#1614): the workflow triggers on every change,
    and its ``changes`` job reads this block to decide whether the heavy jobs run.
    One pattern per line, no quotes.

    Returns ``(start, end, indent, patterns)`` per block, where ``[start, end)`` is
    the line span to replace -- generated comment lines immediately above the key are
    folded into the span so repeated ``--apply`` runs do not stack them up.
    """
    out: list[tuple[int, int, str, list[str]]] = []
    for i, line in enumerate(lines):
        m = re.match(rf"^(?P<indent>\s*){key}:\s*\|\s*$", line)
        if not m:
            continue
        indent = m.group("indent")
        start = i
        while start > 0 and lines[start - 1].lstrip().startswith("#") \
                and MARKER in lines[start - 1]:
            start -= 1
        j, items = i + 1, []
        while j < len(lines):
            body = lines[j]
            pad = len(body) - len(body.lstrip())
            if not body.strip() or pad <= len(indent):
                break
            items.append(body.strip())
            j += 1
        out.append((start, j, indent, items))
    return out


def render(indent: str, key: str, workflow: str, patterns: list[str]) -> list[str]:
    """@brief The generated replacement lines for one ``<key>`` block."""
    lines = [indent + note for note in generated_note(workflow)]
    lines.append(f"{indent}{key}: |")
    lines.extend(f"{indent}  {p}" for p in patterns)
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
        # A reference that resolves to nothing on disk is not an input a path pattern
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


# --- docs.yml: Sphinx, Doxygen and the scripts its steps run ---------------------


def _sphinx_dirs(text: str) -> tuple[str, str]:
    """@brief ``(confdir, srcdir)`` of the workflow's ``sphinx-build -c <conf> <src>``."""
    m = re.search(r"sphinx-build\b[^\n]*?\s-c\s+(\S+)\s+(\S+)", text)
    if not m:
        raise SystemExit("error: docs.yml has no `sphinx-build ... -c <conf> <src>` "
                         "line; update tools/gen_ci_paths.py")
    return m.group(1), m.group(2)


def _include_patterns(conf: pathlib.Path) -> list[str]:
    """@brief The literal ``include_patterns`` list of a Sphinx ``conf.py``."""
    tree = ast.parse(conf.read_text("utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                getattr(t, "id", None) == "include_patterns" for t in node.targets):
            return list(ast.literal_eval(node.value))
    raise SystemExit(f"error: no literal include_patterns in {conf}")


INCLUDE_DIRECTIVE = re.compile(r"\{(?:literalinclude|include)\}\s+(\S+)")


def docs_inputs(text: str) -> tuple[set[str], set[pathlib.Path]]:
    """@brief Directories and files the docs build reads, beyond the core configure.

    * the Sphinx config directory (``-c``) whole, and every source its
      ``include_patterns`` admits;
    * every file a published page pulls in by ``{literalinclude}``/``{include}``;
    * the directory of every script a ``run:`` step executes (a script imports its
      neighbours, so its directory is the honest unit);
    * DOCS_DECLARED.
    """
    confdir, srcdir = _sphinx_dirs(text)
    src = (ROOT / srcdir).resolve()
    dirs = {confdir.strip("/")} | set(DOCS_DECLARED)
    files: set[pathlib.Path] = set()
    for pattern in _include_patterns(ROOT / confdir / "conf.py"):
        if pattern.endswith("/**") and not any(c in pattern[:-3] for c in "*?["):
            dirs.add((src / pattern[:-3]).relative_to(ROOT).as_posix())
            pages = (src / pattern[:-3]).rglob("*.md")
        else:
            pages = src.glob(pattern)
        for page in pages:
            if page.is_file():
                files.add(page.relative_to(ROOT))
    for page in list(files):
        for target in INCLUDE_DIRECTIVE.findall((ROOT / page).read_text("utf-8")):
            base = src if target.startswith("/") else (ROOT / page).parent
            ref = (base / target.lstrip("/")).resolve()
            if ref.is_file() and ROOT in ref.parents:
                files.add(ref.relative_to(ROOT))
    for script in re.findall(r"\b(?:python3|bash|sh)\s+([\w./-]+\.(?:py|sh))\b", text):
        parent = (ROOT / script).resolve().parent
        if (ROOT / script).is_file() and parent != ROOT and ROOT in parent.parents:
            dirs.add(parent.relative_to(ROOT).as_posix())
    for doxyfile in re.findall(r"\bdoxygen\s+(\S+)", text):
        if (ROOT / doxyfile).is_file():
            files.add(pathlib.Path(doxyfile))
    return dirs, files


# --- the gated workflows --------------------------------------------------------

# workflow file -> the env key of its `changes` job's pattern block.
GATES = {
    "core-ci.yml": "CORE_CI_PATHS",
    "docs.yml": "DOCS_PATHS",
}


def derive_all() -> dict[str, list[str]]:
    """@brief The derived list of every gated workflow, self-reference last.

    One configure of the core project serves both: each workflow that runs
    ``cmake -S core`` with ``-DBUILD_TESTING=ON`` consumes everything it references.
    """
    with tempfile.TemporaryDirectory(prefix="libtracer-ci-paths-") as tmp:
        build_dir = pathlib.Path(tmp) / "build"
        configure(build_dir)
        refs = referenced_paths(build_dir)
    out: dict[str, list[str]] = {}
    for name in GATES:
        rel = f"{WORKFLOWS_REL}/{name}"
        text = (ROOT / rel).read_text("utf-8")
        roots = source_roots(text)
        files = set(refs) if CORE_PROJECT in roots else set()
        if name == "docs.yml":
            more_dirs, more_files = docs_inputs(text)
            roots |= more_dirs
            files |= more_files
        patterns = minimal_patterns(roots, files)
        out[rel] = [p for p in patterns if p != rel] + [rel]
    return out


# --- entry point ------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="fail if a committed list is not the derived one")
    mode.add_argument("--apply", action="store_true",
                      help="rewrite every gated workflow's block with its derived list")
    args = ap.parse_args()

    expected_all = derive_all()
    ok = True
    for name, key in GATES.items():
        rel = f"{WORKFLOWS_REL}/{name}"
        path = ROOT / rel
        expected = expected_all[rel]
        lines = path.read_text("utf-8").splitlines()
        blocks = pattern_blocks(lines, key)
        if not blocks:
            print(f"error: no `{key}: |` block found in {rel} — the workflow shape "
                  "changed; update tools/gen_ci_paths.py", file=sys.stderr)
            return 2

        if args.apply:
            for start, end, indent, _ in reversed(blocks):
                lines[start:end] = render(indent, key, name.removesuffix(".yml"),
                                          expected)
            path.write_text("\n".join(lines) + "\n", "utf-8")
            print(f"applied: {len(blocks)} {key} block(s) in {rel} "
                  f"set to {len(expected)} derived pattern(s).")
            continue

        if not args.check:
            print(f"{rel} {key}:")
            for pattern in expected:
                print(f"  {pattern}")
            continue

        for _, _, _, committed in blocks:
            missing = [p for p in expected if p not in committed]
            extra = [p for p in committed if p not in expected]
            if not missing and not extra:
                continue
            ok = False
            for p in missing:
                print(f"ERROR: {rel} {key} is MISSING {p!r} — the workflow skips its "
                      "jobs for a pull request confined to it, yet its jobs read it.",
                      file=sys.stderr)
            for p in extra:
                print(f"ERROR: {rel} {key} carries {p!r}, which nothing the workflow "
                      "reads refers to (or a broader pattern already covers it).",
                      file=sys.stderr)
        if ok:
            print(f"ok: {rel} gates its jobs on all {len(expected)} derived path(s) "
                  f"({len(blocks)} block(s) checked).")
    if not ok:
        print("\nRegenerate with: python3 tools/gen_ci_paths.py --apply",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
