#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Fail a change that adds compiled or otherwise binary content to the tree.

Why this exists (#1359). PR #1356 committed 34 compiled ELF binaries — a whole
`bench/buildA/` tree, ~49 MB — and **every CI job passed.** A human caught it at the
merge gate. `.gitignore` was widened afterwards, but an ignore rule is a convenience
that stops accidental staging; it is not a gate. A build tree the ignore rules do not
anticipate (`out/`, `cmake-build-debug/`, `obj/`, a stray `a.out`) would still sail
through green. History is permanent: a merged binary lives in the pack forever, and
the fix afterwards is a history rewrite of `main`.

What it checks. Not the whole tree — the tree legitimately contains binaries — but the
files a change ADDS or MODIFIES, diffed against the merge base. Each such file is
classified by CONTENT, never by extension:

  1. **Executable / object magic** (ELF, Mach-O, PE, `ar` archive, LLVM bitcode, Java
     class). Rejected unconditionally, allowlist or not: an `input.bin` that is really
     a stripped ELF is the exact evasion an allowlist-first order would wave through.
  2. **The one accepted binary class** — a conformance vector under
     `tests/conformance/vectors/vN/<group>/<case>/{input,reject}.bin`, up to
     `VECTOR_MAX_BYTES`. Matched by PATH SHAPE, not by the `.bin` extension: a `.bin`
     anywhere else, or one nested at the wrong depth, is not a vector.
  3. **Binary content.** Three independent verdicts, any of which convicts: git's own
     (`git diff --numstat` prints `-` for both add and delete counts on a binary), a
     NUL scan of the blob, and a UTF-8 decode. All three are content tests, and they
     are kept separate on purpose — git's verdict is attribute-driven, so a
     `.gitattributes` line marking a path as text would silence it alone; and the NUL
     scan misses a few hundred bytes of high-entropy artifact that happen to hold no
     zero byte, which the decode catches.
  4. **Build-output naming** (`.o`, `.a`, `.so`, `.exe`, `a.out`, a stray `.bin`, …).
     A supplement to the content tests, not a replacement: content is what catches the
     #1356 shape (`bench/buildA/bench_path_label` — no extension at all), and the name
     is what catches the converse, a short artifact whose bytes happen to hold no NUL
     and so read as text to both git and the NUL scan.
  5. **Size ceiling** (`MAX_TEXT_BYTES`). A text file that large is a dump, a vendored
     blob, or a generated artifact, not source. Caught here rather than argued about
     in review.

Usage:

    tools/check_no_binaries.py                       # origin/main...HEAD
    tools/check_no_binaries.py --base <ref> --head <ref>

Exit status is 0 when the diff is clean and 1 when anything trips, with the offending
paths and the reason each one tripped printed to stderr — the failure has to explain
itself to whoever hits it.
"""
import argparse
import codecs
import re
import subprocess
import sys

# The single accepted binary class in this tree: a protocol conformance vector. The
# suite lays every case out as `vectors/<version>/<group>/<case>/`, holding an
# `input.bin` (or `reject.bin` for a must-reject case) beside its `expected.json` and
# `description.md`. Anchored and depth-exact so that neither a `.bin` dropped
# elsewhere nor one buried deeper inside the vector tree inherits the exemption.
VECTOR_PATTERN = re.compile(
    r"^tests/conformance/vectors/v[0-9]+/[^/]+/[^/]+/(?:input|reject)\.bin$"
)

# Vectors are frames, and a frame is small — the largest in the tree is ~2 KB. The
# ceiling is deliberately snug: it is the second lock on the exemption, so that
# "call it input.bin" cannot become a way to smuggle bulk content past rule 2.
VECTOR_MAX_BYTES = 8 * 1024

# Nothing this change-set adds should be this big. Vendored data or a large fixture is
# a deliberate decision that belongs in review, so it fails here and gets discussed.
MAX_TEXT_BYTES = 1024 * 1024

# Leading bytes that identify a compiled artifact. Every one of these is something a
# build produced, and none of them has a legitimate reason to enter this tree.
EXECUTABLE_MAGIC = (
    (b"\x7fELF", "ELF executable / shared object / relocatable object (or core dump)"),
    (b"!<arch>\n", "ar archive (static library, .a)"),
    (b"MZ", "DOS/PE executable (.exe / .dll)"),
    (b"\xfe\xed\xfa\xce", "Mach-O executable (32-bit)"),
    (b"\xfe\xed\xfa\xcf", "Mach-O executable (64-bit)"),
    (b"\xce\xfa\xed\xfe", "Mach-O executable (32-bit, byte-swapped)"),
    (b"\xcf\xfa\xed\xfe", "Mach-O executable (64-bit, byte-swapped)"),
    (b"\xca\xfe\xba\xbe", "Mach-O universal binary or Java class file"),
    (b"BC\xc0\xde", "LLVM bitcode"),
)

# git's own binary heuristic looks at the first 8000 bytes for a NUL; the scan below
# mirrors it so the two verdicts are comparable.
NUL_SCAN_BYTES = 8000

# Names that only a build ever produces. This is a SUPPLEMENT to the content tests
# above, never a substitute for them — the content tests are what catch a build tree
# with no extension at all (`bench/buildA/bench_path_label`, the #1356 shape), and
# this catches the converse: a short artifact whose bytes happen to contain no NUL
# and so reads as text to both git and the NUL scan. An empty `.o` is still a `.o`.
ARTIFACT_NAME = re.compile(
    r"(?:^|/)(?:a\.out|core(?:\.[0-9]+)?)$"
    r"|\.(?:o|obj|a|lib|so|dylib|dll|exe|elf|bin|ko|pyc|pyo|class|gch|pch|"
    r"lo|la|dSYM|gcda|gcno|profraw)$"
    r"|\.so\.[0-9.]+$"
)


def is_vector_path(path):
    """True when `path` is the one accepted binary class: a conformance vector."""
    return bool(VECTOR_PATTERN.match(path))


def executable_magic(blob):
    """Return the human name of the compiled format `blob` starts with, else None."""
    for magic, name in EXECUTABLE_MAGIC:
        if blob.startswith(magic):
            return name
    return None


def decodes_as_utf8(chunk):
    """True when `chunk` is valid UTF-8, tolerating a codepoint split at its end.

    The NUL test alone lets one shape through: a few hundred bytes of high-entropy
    artifact that happen to contain no zero byte. Git calls that text and so does the
    NUL scan. Source does not look like that — every text file in this tree decodes —
    so a decode failure is the last content test.
    """
    decoder = codecs.getincrementaldecoder("utf-8")()
    try:
        decoder.decode(chunk, False)
    except UnicodeDecodeError:
        return False
    return True


def classify(path, blob, git_says_binary):
    """Judge one added/modified file. Return a reason string, or None when it is fine.

    `blob` is the file's full content at the head revision and `git_says_binary` is
    git's own numstat verdict. The order is load-bearing: the compiled-artifact test
    runs BEFORE the conformance-vector exemption, so a vector-shaped path cannot
    launder an ELF.
    """
    fmt = executable_magic(blob)
    if fmt is not None:
        return "compiled artifact — {}, identified by its leading magic bytes".format(
            fmt
        )

    size = len(blob)
    if is_vector_path(path):
        if size > VECTOR_MAX_BYTES:
            return (
                "conformance vector is {} bytes, over the {}-byte vector ceiling — "
                "vectors are single frames; this is bulk content wearing a vector's "
                "path".format(size, VECTOR_MAX_BYTES)
            )
        return None

    if ARTIFACT_NAME.search(path):
        return (
            "named like build output — only a conformance vector "
            "(tests/conformance/vectors/vN/<group>/<case>/{input,reject}.bin) may "
            "carry this kind of name"
        )

    if git_says_binary:
        return (
            "binary content — git reports no textual diff for it, and only "
            "conformance vectors (tests/conformance/vectors/vN/<group>/<case>/"
            "{input,reject}.bin) may be binary"
        )

    if b"\x00" in blob[:NUL_SCAN_BYTES]:
        return (
            "binary content — a NUL byte appears in the first {} bytes (git was "
            "told to treat the path as text)".format(NUL_SCAN_BYTES)
        )

    if not decodes_as_utf8(blob[:NUL_SCAN_BYTES]):
        return (
            "binary content — the first {} bytes are not valid UTF-8, so this is "
            "not source. Every text file in this tree decodes; a high-entropy "
            "artifact that happens to contain no NUL does not".format(NUL_SCAN_BYTES)
        )

    if size > MAX_TEXT_BYTES:
        return "{} bytes, over the {}-byte ceiling for a single added file".format(
            size, MAX_TEXT_BYTES
        )

    return None


def _git(args, repo=None, binary=False):
    """Run git and return its stdout (text unless `binary`)."""
    out = subprocess.run(
        ["git"] + (["-C", repo] if repo else []) + args,
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    return out if binary else out.decode("utf-8", "surrogateescape")


def changed_files(base, head, repo=None):
    """Map every added/modified/renamed path in `base...head` to git's binary verdict.

    `git diff --numstat -z` is git's own binary marker: a binary file prints `-` for
    both the added and the deleted line count. With `-z` a rename emits an empty path
    field followed by the pre- and post-image paths, so those records are stitched
    back together here; only the post-image (the path that will exist on `main`)
    is judged.
    """
    raw = _git(
        ["diff", "--numstat", "-z", "--diff-filter=ACMR", "{}...{}".format(base, head)],
        repo=repo,
    )
    fields = [f for f in raw.split("\0") if f != ""] if raw else []
    result = {}
    i = 0
    while i < len(fields):
        added, deleted, path = fields[i].split("\t", 2)
        if path == "":
            # Rename/copy: this record's path field is empty and the next two fields
            # are the pre-image and post-image paths.
            path = fields[i + 2]
            i += 3
        else:
            i += 1
        result[path] = added == "-" and deleted == "-"
    return result


def check(base, head, repo=None):
    """Return an ordered list of `(path, reason)` for everything that trips."""
    offenders = []
    for path, git_says_binary in sorted(changed_files(base, head, repo).items()):
        blob = _git(["show", "{}:{}".format(head, path)], repo=repo, binary=True)
        reason = classify(path, blob, git_says_binary)
        if reason is not None:
            offenders.append((path, reason))
    return offenders


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--base",
        default="origin/main",
        help="the revision to diff against (default: origin/main)",
    )
    parser.add_argument(
        "--head", default="HEAD", help="the revision under test (default: HEAD)"
    )
    parser.add_argument("--repo", default=None, help="run against this checkout")
    args = parser.parse_args(argv)

    offenders = check(args.base, args.head, args.repo)
    if not offenders:
        print(
            "no committed binaries: every file added or modified in {}...{} is "
            "text or an accepted conformance vector".format(args.base, args.head)
        )
        return 0

    print(
        "This change commits binary content. Compiled output must never enter the "
        "history — it is permanent once merged, and removing it means rewriting "
        "main. See #1359.",
        file=sys.stderr,
    )
    print("", file=sys.stderr)
    for path, reason in offenders:
        print("  {}\n      {}".format(path, reason), file=sys.stderr)
    print("", file=sys.stderr)
    print(
        "If you built in-tree, delete the build directory and amend; if you believe "
        "one of these is legitimate content this repo must carry, say so in the PR "
        "and widen the allowlist in tools/check_no_binaries.py in the same change.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
