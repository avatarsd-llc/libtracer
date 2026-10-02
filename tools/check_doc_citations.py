#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Verify that the docs' code citations still name what they claim.

The documentation cites source code — CONTEXT.md (the canonical glossary), the reference
and interop pages, the READMEs. Every such citation is a SYMBOL citation (#1705, #1706):
`` `core/src/graph.cpp:graph_t::write_impl` `` names the code by what it IS, and
@ref symbol_citations checks it by SEARCH. It fails, naming the citing page, when the
symbol is GONE from the cited file or names MORE THAN ONE place in it. An edit above the
cited code never touches the citation; a rename or a removal turns the gate red.

LINE-NUMBER citations (`graph.cpp:956`, `graph.cpp:12-20`, a bare `` `:371` `` continuing
the file named before it) are REFUSED in every living doc (@ref line_citations). They
used to be the only spelling, pinned by a hand-kept anchor table and moved by a re-pin
tool after every edit that shifted a cited file — about a quarter of the non-merge
commits between August and October 2026 were nothing but re-pins. #1706 migrated the last
of them and deleted the table and the tool, so no code edit can force a citation-only
commit any more.

Spellings, as before (#803): a full repo-relative path, the design pages' BASENAME
shorthand (`graph.hpp:...`), or a partial path that singles out one carrier. A basename
carried by two files is an ERROR, not a guess — the doc must spell the full path. A token
that names no file in the tree is not a citation at all (`127.0.0.1:47301`,
`wss://robot.local:9000`), and a markdown page cited by line (`07-host-embedding.md:79`)
is a pointer into prose, which this gate has never checked.

Historical genres are deliberately NOT checked. `docs/adr/`, `docs/spec/` and
`docs/research/` are dated records of a decision: their citations describe the tree as it
stood, line numbers included, and rewriting them would rewrite the record.

Usage:  python3 tools/check_doc_citations.py
Exits non-zero when any living doc carries a line-number citation or a symbol citation
that does not resolve to exactly one place, listing every one it found.
Gated by `.github/workflows/doc-citations.yml`; unit tests in
`tools/tests/test_check_doc_citations.py`.
"""

import functools
import os
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# Extensions a citation may name. `.hpp.in` is the CMake-configured header template
# (`config.hpp`) — the only place the compile-time knobs are *declared*, and what
# the configuration pages therefore cite; the generated `config.hpp` is a build
# artifact and is not in the tree.
SOURCE_SUFFIXES = (".hpp.in", ".hpp", ".cpp", ".cc", ".hh", ".h")

# Directories that hold no citable source: build output, vendored deps, worktrees.
# `.pio` is PlatformIO's per-project cache: `pio run` in the packaging fixture unpacks the
# library UNDER TEST into `.pio/libdeps/<env>/libtracer/`, which is a second copy of every
# core source. Without this the gate turns red the moment anyone runs that fixture locally,
# for the same reason `build-` is here — a basename with two paths is ambiguous, and the
# gate must not depend on which builds someone happens to have run in the tree.
NON_SOURCE_DIRS = (
    "_build", "build", "node_modules", ".claude", ".git", "target", "dist", ".pio",
)
# ...and the same, for any `build-<something>` sibling. The exact name "build" is not the
# only one that appears: this repo's agent workflow mandates `build-agent` (`.gitignore`
# covers `build-*/`), and a generated `build-agent/generated/include/libtracer/config.hpp`
# makes `config.hpp` an AMBIGUOUS basename, which the gate reports as stale citations that
# do not exist. Green-vs-red then depended on whether someone had configured a build in the
# tree, which is exactly the kind of environment-sensitivity a gate must not have.
#
# `bench-` is here for the identical reason (#1050): the bench tree is configured separately
# from the core one and so cannot reuse an occupied `build-*` name, which makes `bench-agent`
# the second sanctioned prefix in `.gitignore`. A `cmake -S bench -B bench-agent` renders the
# same `generated/.../config.hpp` one level deeper and turned this gate red. This tuple and
# the `.gitignore` prefix list describe the same set and have to be changed together.
NON_SOURCE_DIR_PREFIXES = ("build-", "bench-")


def _is_non_source_part(part: str) -> bool:
    """@brief True if a path component names a directory holding no citable source."""
    return part in NON_SOURCE_DIRS or part.startswith(NON_SOURCE_DIR_PREFIXES)


def source_map(root: pathlib.Path = None) -> dict:
    """Map every source BASENAME in the tree to the repo-relative paths carrying it.

    A basename with one path is resolvable shorthand; a basename with two or more is
    ambiguous and a doc citing it is an error, not a coin flip. Built once per run.
    """
    root = root or REPO
    out = {}
    for path in root.rglob("*"):
        if not path.is_file() or not path.name.endswith(SOURCE_SUFFIXES):
            continue
        rel = path.relative_to(root)
        if any(_is_non_source_part(p) for p in rel.parts):
            continue
        out.setdefault(path.name, []).append(rel.as_posix())
    return {name: sorted(paths) for name, paths in out.items()}


# One token: an optional directory prefix, a basename, `:`, and what follows. The first four
# branches are the LINE-NUMBER spellings, kept only so @ref line_citations can refuse them:
# a source file (longest extension first, so `config.hpp.in` never truncates to
# `config.hpp`), a DOCUMENT (which ends a bare-continuation run and is otherwise ignored), a
# bare `` `:99` `` continuation, and any other `path.ext` (classified after resolving: a
# workflow, a CMakeLists, a `.mjs` driver — or an address, which names no file).
#
# The leading `` `? `` on the DOCUMENT branch is load-bearing: `finditer` takes the EARLIEST
# match, and only then the earliest alternative, so without it a backticked
# `` `docs/reference/07-host-embedding.md:79` `` matched the catch-all one character earlier
# and stopped ending the run.
#
# The last branch is the SYMBOL citation (#1705): backticked on both sides (a prose
# `file.h: No such file` is not a citation), the text after the colon starts with neither a
# digit nor a space, which keeps it disjoint from every line-numbered branch, and it may wrap
# onto ONE more line, as a Markdown code span can.
_EXTS = "|".join(re.escape(s[1:]) for s in SOURCE_SUFFIXES)
DOC_EXTS = "md|rst"
_ANY_PATH = r"(?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.[A-Za-z0-9_]+"
CITATION_RE = re.compile(
    r"`?((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + _EXTS + r")):([\d,\-]+)`?"
    r"|`?((?:[A-Za-z0-9_./-]*/)?[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:" + DOC_EXTS + r")):[\d,\-]+"
    r"|`:([\d,\-]+)`"
    r"|`?(?P<other>" + _ANY_PATH + r"):(?P<otherspec>[\d,\-]+)`?"
    r"|`(?P<sympath>" + _ANY_PATH + r"):(?P<symbol>[^\s`\d][^`\n]*(?:\n[^`\n]*)?)`"
)


@functools.lru_cache(maxsize=None)
def tree_index() -> dict:
    """Map every basename in the tree to its repo-relative paths (build output excluded).

    It answers one question for a NON-source spelling — "does this token name a REAL
    FILE?" — which is what separates a citation from a host:port pair. `tests/testbed/README.md` writes
    `127.0.0.1:47301` and `bindings/.../README.md` writes `wss://robot.local:9000`; both
    parse as `path.ext:digits` and neither is a citation. Nothing in the tree is named
    `127.0.0.1` or `robot.local`, and that is the whole discriminator.

    Walks with `os.walk` and PRUNES as it goes, rather than `rglob`-ing everything and
    filtering after. Same result, and the difference is not cosmetic: `rglob` descends into
    `.git` and every `build-*` tree before discarding them, which measured +1.2 s on a gate
    that runs ~1.5 s. Pruning keeps this second walk in the noise.
    """
    out = {}
    for dirpath, dirnames, filenames in os.walk(REPO):
        dirnames[:] = [d for d in dirnames if not _is_non_source_part(d)]
        rel_dir = pathlib.Path(dirpath).relative_to(REPO).as_posix()
        for name in filenames:
            # A FILE whose own name is a non-source part is skipped too, matching what
            # @ref source_map's `rel.parts` test does (it sees the basename as a part).
            if _is_non_source_part(name):
                continue
            out.setdefault(name, []).append(name if rel_dir == "." else f"{rel_dir}/{name}")
    return {name: sorted(paths) for name, paths in out.items()}


@functools.lru_cache(maxsize=None)
def _resolve(spelling: str, filemap_key: tuple) -> tuple:
    """Resolve one cited spelling to (repo-relative path, error-or-None)."""
    filemap = dict(filemap_key)
    hits = filemap.get(spelling.rsplit("/", 1)[-1], ())
    if "/" in spelling:
        # A spelled path is the doc's answer to ambiguity, so it is honoured FIRST: a
        # partial path that singles out one of the basename's carriers resolves to it,
        # and only a spelling that still names two of them is an error. A path the map
        # does not carry at all falls back to the filesystem, then to the basename.
        exact = [h for h in hits if h == spelling or h.endswith("/" + spelling)]
        if len(exact) == 1:
            return exact[0], None
        if not exact and (REPO / spelling).is_file():
            return spelling, None
        hits = exact or hits
    if len(hits) == 1:
        return hits[0], None
    if len(hits) > 1:
        return None, f"`{spelling}` is an ambiguous basename ({', '.join(hits)}) — cite the full path"
    return None, None


# --------------------------------------------------------------------------------------
# Symbol citations (#1705): name the code, not its line number
# --------------------------------------------------------------------------------------
#
# A line citation is a pointer into a moving file, so every edit above it cost a re-pin
# commit — about a quarter of the non-merge commits from August 2026 were nothing else. A
# SYMBOL citation, `` `graph.cpp:graph_t::propagate` ``, names what the code IS, and is
# checked by SEARCH: it resolves when the cited file holds that symbol exactly once, and it
# fails, naming the citing page, when the symbol is gone or names more than one place. No
# anchor, no re-pin, and a rename still turns the gate red.
#
# Two spellings, told apart by shape:
#
# * A SYMBOL — an identifier, optionally qualified (`graph_t::propagate`, `tr::wire::decode`,
#   `kVertexLockStripes`), optionally with a trailing `()` to say "the function". It
#   matches whole identifiers in CODE only (comments and string literals are blanked first),
#   so a stale comment that still mentions a renamed symbol does not keep it alive. A
#   qualified symbol also matches its out-of-line spelling with template arguments
#   (`graph_t<C>::propagate`). When it occurs more than once — a member is declared once and
#   USED many times — the occurrences are narrowed to DECLARATIONS (@ref _is_declaration),
#   and a qualified symbol further to the declarations whose innermost enclosing
#   class/struct/namespace is its last qualifier.
# * A SUBSTRING — anything else (`if (depth > kMaxDepth)`, `struct opt_t`). Matched
#   literally against the raw lines, comments included, and must sit on exactly one line.
#   This is the escape hatch when a symbol is genuinely ambiguous (overloads) or when the
#   sentence is about one statement rather than one declaration.
#
# The narrowing is a heuristic over text, not a parser. Its failure mode is LOUD — a
# symbol it cannot single out is reported as ambiguous, and the author qualifies it or
# cites a longer substring — never a silent pick.
SYMBOL_RE = re.compile(r"^~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*$")
# Files whose comments and literals are blanked before a SYMBOL is searched for.
C_LIKE_SUFFIXES = SOURCE_SUFFIXES
# A token that, written immediately before a name, makes it a declaration of that name.
_DECL_KEYWORDS = frozenset(
    ("class", "struct", "union", "enum", "namespace", "using", "typedef", "define", "concept"))
# Identifiers that end an EXPRESSION prefix rather than a type: `return foo(x)` calls foo.
_NON_TYPE_WORDS = frozenset((
    "return", "else", "case", "throw", "co_return", "co_yield", "co_await", "delete", "new",
    "goto", "sizeof", "alignof", "decltype", "typeid", "not", "and", "or", "if", "while",
    "for", "switch", "do", "noexcept", "requires", "static_assert",
))
_SCOPE_OPENER_RE = re.compile(
    r"\b(?:class|struct|union|namespace|enum(?:\s+(?:class|struct))?)\s+"
    r"(?:\[\[[^\]]*\]\]\s*)?([A-Za-z_][\w:]*)")
_LEXEME_RE = re.compile(r'/\*|//|"|\'')
_STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])\'')


def _code_lines(lines: list) -> list:
    """`lines` with comments and string/char literals blanked — the text a SYMBOL is
    searched in. Line count and numbering are preserved; block comments may span lines."""
    out, in_block = [], False
    for line in lines:
        text, i, code = line, 0, []
        while i < len(text):
            if in_block:
                end = text.find("*/", i)
                if end < 0:
                    i = len(text)
                    break
                in_block, i = False, end + 2
                continue
            m = _LEXEME_RE.search(text, i)
            if not m:
                code.append(text[i:])
                break
            code.append(text[i:m.start()])
            tok = m.group()
            if tok == "//":
                break
            if tok == "/*":
                in_block, i = True, m.end()
                continue
            lit = _STRING_RE.match(text, m.start())
            if lit:
                code.append(" ")
                i = lit.end()
            else:
                code.append(tok)  # a digit separator (`1'000`) or a stray quote
                i = m.end()
        out.append("".join(code))
    return out


def _scopes(code: list) -> list:
    """For each line, the stack of enclosing brace scopes AT ITS START, innermost last.

    A scope opened by `class`/`struct`/`union`/`enum`/`namespace` carries that name (the
    last component, so `namespace tr::wire` reads as `wire`); every other block (a function
    body, a lambda, an initializer) carries None.
    """
    stack, pending, out = [], "", []
    for line in code:
        out.append(tuple(stack))
        for ch in line:
            if ch == "{":
                names = _SCOPE_OPENER_RE.findall(pending)
                stack.append(names[-1].rsplit("::", 1)[-1] if names else None)
                pending = ""
            elif ch == "}":
                if stack:
                    stack.pop()
                pending = ""
            elif ch == ";":
                pending = ""
            else:
                pending += ch
        pending += " "
    return out


def _is_declaration(code: list, idx: int, start: int, end: int) -> bool:
    """True when the name at `code[idx][start:end]` is being DECLARED, not used.

    The rule is the shape of a C++ declaration: the name is preceded by a type (an
    identifier that is not an expression keyword, or `>`, `*`, `&`, `]`) or by a declaring
    keyword (`class`, `using`, `#define`, ...), and followed by what can follow a declarator
    (`(`, `;`, `{`, `=`, `[`, `,`, a single `:`, or the end of the line). An enumerator has
    no type, so a name opening a line that ends in `,` or carries no `;` counts too. A
    declaration whose return type sits on the line above reads its prefix from there.
    """
    line = code[idx]
    prefix, suffix = line[:start].rstrip(), line[end:].lstrip()
    # Inside an open parameter list the name is a PARAMETER (`slot_server_t(bool peer_named)`)
    # and declares nothing a citation can mean.
    if prefix.count("(") > prefix.count(")"):
        return False
    if not prefix:
        if re.match(r"(?:=(?!=)|,|$)", suffix) and (line.rstrip().endswith(",") or ";" not in line):
            return True
        j = idx - 1
        while j >= 0 and not code[j].strip():
            j -= 1
        prefix = code[j].rstrip() if j >= 0 else ""
    # An out-of-line definition, `result_t<void> graph_t<C>::write(`: the qualifier chain is
    # part of the declarator, so the type that decides it sits before the chain.
    prefix = re.sub(r"(?:~?[A-Za-z_]\w*(?:<[^;{}()]*>)?\s*::\s*)+$", "", prefix).rstrip()
    # ...and so do the attribute specifiers a type head may carry: `struct alignas(N) x_t`.
    prefix = re.sub(r"(?:\s*(?:alignas\([^)]*\)|\[\[[^\]]*\]\]))+$", "", prefix).rstrip()
    if not re.match(r"(?:\(|;|\{|=(?!=)|\[|,|:(?!:)|$|final\b|override\b|const\b|noexcept\b)",
                    suffix):
        return False
    # A pointer or reference declarator (`mem_backend_t* value_backend_`) is a declaration
    # only when a TYPE stands before the `*`/`&`; `return *value_backend_` is a use.
    m = re.search(r"([A-Za-z_]\w*|\]\]|(?<!-)>)\s*[*&]*$", prefix)
    if not m:
        return False
    tok = m.group(1)
    if tok in _DECL_KEYWORDS:
        return True
    return tok not in _NON_TYPE_WORDS


def symbol_hits(lines: list, needle: str, c_like: bool = True) -> list:
    """Every 1-based line of `lines` the citation text `needle` resolves to (see above).

    One hit is a resolved citation; none is a GONE symbol; more than one is ambiguous. An
    OVERLOADED function is singled out by the start of its parameter list —
    `graph_t::unsubscribe(subscription_t` — which keeps only the declarations whose name is
    followed by that text (whitespace-insensitive).
    """
    head, paren, args = needle.partition("(")
    if not SYMBOL_RE.match(head):
        return [i + 1 for i, ln in enumerate(lines) if needle in ln]
    args = "" if args in ("", ")") else args
    code = _code_lines(lines) if c_like else lines
    parts = head.split("::")
    literal = re.compile(r"(?<![\w~])" + r"(?:<[^;{}()]*>)?::".join(map(re.escape, parts))
                         + r"(?!\w)")
    hits = [i + 1 for i, ln in enumerate(code) if literal.search(ln)]
    if len(hits) == 1 and len(parts) == 1 and not args:
        return hits
    # Narrow to DECLARATIONS of the last component. A qualified symbol always takes this
    # path, even with one literal hit: `return slot_server_t::peer_named();` is a use, and the
    # declaration it names sits unqualified inside `class slot_server_t`.
    name = re.compile(r"(?<![\w~])" + re.escape(parts[-1]) + r"(?!\w)")
    scopes = _scopes(code) if len(parts) > 1 else None
    decls = []
    for i, ln in enumerate(code):
        for m in name.finditer(ln):
            # A constructor (`graph_t::graph_t`) has no type before it, so the declaration
            # rule cannot see it; inside its own class, a name opening a call-shaped line is it.
            ctor = (len(parts) > 1 and parts[-1] == parts[-2]
                    and re.match(r"\s*(?:(?:explicit|constexpr|inline)\s+)*$", ln[:m.start()])
                    and ln[m.end():].lstrip().startswith("("))
            if not ctor and not _is_declaration(code, i, m.start(), m.end()):
                continue
            if scopes is not None:
                # Qualified AT this occurrence (an out-of-line definition), or declared
                # directly inside the scope the last qualifier names.
                inner = scopes[i][-1] if scopes[i] else None
                here = any(lm.end() == m.end() for lm in literal.finditer(ln))
                if not here and inner != parts[-2]:
                    continue
            decls.append(i + 1)
            break
    # A type's name is also its constructors' name. Cited bare (`bump_source_t`) it means the
    # TYPE, so a class/struct/union/enum definition wins over the constructors declared in
    # it; `bump_source_t::bump_source_t` still names a constructor.
    if not (len(parts) > 1 and parts[-1] == parts[-2]):
        types = [h for h in decls
                 if re.search(r"\b(?:class|struct|union|enum)(?:\s+(?:class|struct))?"
                              r"(?:\s+alignas\([^)]*\)|\s+\[\[[^\]]*\]\])*\s+"
                              + re.escape(parts[-1]) + r"(?!\w)", code[h - 1])
                 and not code[h - 1].rstrip().endswith(";")]
        decls = types or decls
    found = decls or hits
    if args:
        want = re.sub(r"\s+", "", args)
        # The parameter list may wrap, so the text after the name runs on for two lines.
        found = [h for h in found
                 if any(re.sub(r"\s+", "", " ".join([code[h - 1][m.end():]] + code[h:h + 2]))
                        .startswith("(" + want)
                        for m in name.finditer(code[h - 1]))]
        # `static_assert(sizeof(void*) != 8` has the same shape and is a SUBSTRING: when no
        # declaration takes the parameter text, the needle is matched literally instead.
        if not found:
            return [i + 1 for i, ln in enumerate(lines) if needle in ln]
    return found


def _symbol_target(spelling: str, key: tuple) -> tuple:
    """Resolve the FILE of a symbol citation, as (repo-relative path, error-or-None).

    A source spelling goes through @ref _resolve, so the basename shorthand and the
    ambiguity rule hold — and a source-suffixed spelling naming no file is an ERROR here,
    because a symbol citation exists only to be checked. Any other spelling must name exactly
    one file in the tree; one that names none (an address, `0.0.0.0:<port>`) is not a
    citation at all.
    """
    if spelling.endswith(SOURCE_SUFFIXES):
        resolved, err = _resolve(spelling, key)
        if resolved or err:
            return resolved, err
        return None, f"`{spelling}` names no source file in the tree"
    return _tree_file(spelling)


def _tree_file(spelling: str) -> tuple:
    """Resolve a NON-source spelling against the whole tree, as (path, error-or-None)."""
    hits = tree_index().get(spelling.rsplit("/", 1)[-1], ())
    if "/" in spelling:
        hits = [h for h in hits if h == spelling or h.endswith("/" + spelling)]
    if len(hits) > 1:
        return None, f"`{spelling}` is an ambiguous basename ({', '.join(hits)}) — cite the full path"
    return (hits[0], None) if hits else (None, None)


def symbol_citations(text: str, filemap: dict = None) -> list:
    """Every symbol citation in one doc that does not resolve to exactly one place.

    The verify pass for the #1705 spelling: no line number — the cited file is
    searched (@ref symbol_hits), and a GONE or AMBIGUOUS symbol is reported with the
    candidate lines, so the author can see what the citation now names.
    """
    filemap = source_map() if filemap is None else filemap
    key = tuple((k, tuple(v)) for k, v in sorted(filemap.items()))
    out, contents = [], {}
    for m in CITATION_RE.finditer(text):
        spelling, needle = m.group("sympath"), m.group("symbol")
        if not spelling:
            continue
        # A code span may wrap onto the next line; Markdown reads that break as a space.
        needle = " ".join(needle.split())
        path, err = _symbol_target(spelling, key)
        if err:
            out.append(err)
        if not path:
            continue
        if path not in contents:
            try:
                contents[path] = (REPO / path).read_text().split("\n")
            except (OSError, UnicodeDecodeError):
                contents[path] = None
        lines = contents[path]
        if lines is None:
            out.append(f"`{spelling}:{needle}` cites a file this gate cannot read ({path})")
            continue
        hits = symbol_hits(lines, needle, path.endswith(C_LIKE_SUFFIXES))
        if not hits:
            out.append(f"`{spelling}:{needle}` — {needle!r} is GONE from {path}: the symbol was "
                       f"renamed or removed. Re-point the citation at what the sentence "
                       f"describes now.")
        elif len(hits) > 1:
            out.append(f"`{spelling}:{needle}` is AMBIGUOUS — {needle!r} names {len(hits)} "
                       f"places in {path} (lines {hits[:8]}). Qualify it (`type_t::name`), "
                       f"or cite a longer substring that occurs once.")
    return out


def line_citations(text: str, filemap: dict = None) -> list:
    """Every LINE-NUMBER citation in one doc, as refusal messages (#1706).

    A line number is a pointer into a moving file: every edit above it made the citation
    wrong, and keeping it right took an anchor table and a re-pin commit. Refused in every
    spelling — a source file by full path or basename, any other file in the tree (a
    workflow, a CMakeLists), a range, a comma list, and a bare `` `:N` `` that continues the
    file named by the citation before it (a symbol citation of a source file starts such a
    run, exactly as a line citation did). A cited DOCUMENT ends the run and is not refused:
    a pointer into prose is not a pointer into code. A token naming no file in the tree is
    not a citation, and a basename two files carry is reported as ambiguous.
    """
    filemap = source_map() if filemap is None else filemap
    key = tuple((k, tuple(v)) for k, v in sorted(filemap.items()))
    out, last = [], None
    for m in CITATION_RE.finditer(text):
        if m.group("sympath"):
            spelling = m.group("sympath")
            last = _resolve(spelling, key)[0] if spelling.endswith(SOURCE_SUFFIXES) else last
            continue
        if m.group(3):
            last = None  # a cited DOCUMENT ends the run
            continue
        if m.group(1):
            path, err = _resolve(m.group(1), key)
            if err:
                out.append(err)
            last = path
            if not path:
                continue
            spec = m.group(2)
        elif m.group("other"):
            path, err = _tree_file(m.group("other"))
            if err:
                out.append(err)
            if not path:
                continue  # an address or an out-of-tree file — not a citation
            spec = m.group("otherspec")
        elif last:
            path, spec = last, m.group(4)
        else:
            continue
        if not any(part.split("-")[0].isdigit() for part in spec.split(",")):
            continue
        out.append(f"`{path}:{spec.strip(',-')}` is a line-number citation — cite the code "
                   f"by symbol instead (`{path}:<symbol>`, or a substring that occurs once), "
                   f"so an edit above it cannot make it wrong")
    return out


HISTORICAL_GENRES = ("docs/adr/", "docs/spec/", "docs/research/")


def is_historical(rel: str) -> bool:
    """True when `rel` (a repo-relative posix path) is a dated record, not a live doc."""
    return rel.startswith(HISTORICAL_GENRES)


def all_docs() -> list:
    """Every tracked markdown file. `_build` is generated Sphinx output; the
    `.claude/worktrees/fw-pin-*` trees are pinned history. Neither is a source."""
    skip = ("_build", "node_modules", ".claude", ".git")
    # Match skip components against the path RELATIVE to the repo root — an absolute
    # match made the tool skip EVERY doc when run from a `.claude/worktrees/*` checkout,
    # turning the whole check into a vacuous pass there.
    return [p for p in REPO.rglob("*.md")
            if not any(s in p.relative_to(REPO).parts for s in skip)]


def main(argv: list = None) -> int:
    """Check every living doc; print each failure and return the exit status."""
    if argv:
        print(f"usage: {sys.argv[0]} (no arguments)", file=sys.stderr)
        return 2
    filemap = source_map()
    failures, checked = [], 0
    for doc in all_docs():
        rel = doc.relative_to(REPO).as_posix()
        if is_historical(rel):
            continue
        try:
            text = doc.read_text()
        except (OSError, UnicodeDecodeError):
            continue
        checked += sum(1 for m in CITATION_RE.finditer(text) if m.group("sympath"))
        failures += [f"{rel}: {e}" for e in dict.fromkeys(line_citations(text, filemap))]
        failures += [f"{rel}: {e}" for e in dict.fromkeys(symbol_citations(text, filemap))]
    for f in failures:
        print(f"FAIL  {f}")
    if failures:
        print(f"\n{len(failures)} citation problem(s) in the docs.")
        return 1
    print(f"OK    {checked} symbol citations verified; no line-number citations.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
