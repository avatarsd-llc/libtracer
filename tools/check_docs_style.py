#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""The docs style lint: one page format for the whole site (#2374, #2371).

Every rule here is one the unified docs format names, and the docs-article skill
(`.claude/skills/docs-article/SKILL.md`) is where a writer reads them. A hit names its
page, its line and its rule:

- `alt-text`: a `{mermaid}` directive without `accTitle:` and `accDescr`, an image
  with empty alt text, or an `{image}`/`{figure}` without `:alt:`. Bare ```` ```mermaid ````
  fences are kept for GitHub and not linted (ruling 6).
- `diagram-style`: a `style`, `classDef` or `linkStyle` line in a Mermaid diagram, bare
  fence or directive. Colour comes from the vocabulary classes in
  `docs/_static/diagram-vocabulary.css`, never from the page (#2373).
- `concept-sections`: a page under `docs/concepts/` that lacks a required template
  heading, puts the template headings out of order, adds a heading of its own, or has a
  `## Cost` without a `### Latency`. `docs/concepts/index.md` is navigation, not a
  concept, and is exempt.
- `normative-source`: a "Normative source" field that names anything but v1 or an annex.
- `exercised-by`: an "Exercised by" field that names a vector or test that does not exist.
- `line-length`: a prose line over 100 characters, link targets not counted.
- `own-site-link`: an absolute link to one of our own published pages.
- `restatement`: an RFC 2119 keyword (MUST, SHALL, SHOULD, REQUIRED) stated outside the
  spec. Guides link the clause instead of restating it.

The spec itself is never linted: v1, the annexes v1 §3 incorporates (read from v1 itself,
the only list of them), and the RFCs under `docs/spec/rfcs/`, which are records. Format
work leaves those pages alone; they change by erratum or amendment.
- `avoid-phrase`: a phrase from `CONTEXT-AVOID.md`, the short list beside the glossary.
- `sidebar-order`: `index.md`'s toctree captions, which are the sidebar's sections, must be
  exactly the reader-intent order (#2381): Start here, Learn, Concepts, Recipes, Reference,
  Specification, Decisions, Evidence. Order is set here, never by renaming a file.
- `annex-banner`: an annex v1 §3 lists that does not open with the one-line annex banner
  under its title, or any other page that carries it (#2385). This is the one rule that
  reads an annex, and the unit tests hold the real tree to zero hits of it.

Code blocks and code spans are quoted text and never linted, except that `alt-text`
reads the diagram directives themselves.

The lint is REPORT-ONLY against a committed baseline (`tools/docs_style_baseline.json`,
hits counted per page and rule): it prints every hit, marks the page/rule counts that
exceed the baseline as NEW, and exits 0. `--gate` makes a NEW count fail (#2382 switches
it on in the docs workflow); `--update-baseline` records today's hits.

Usage:  python3 tools/check_docs_style.py [--gate] [--baseline FILE] [--update-baseline]
Unit tests in `tools/tests/test_check_docs_style.py`. Standard library only.
"""
from __future__ import annotations

import argparse
import ast
import dataclasses
import fnmatch
import json
import pathlib
import posixpath
import re
import subprocess
import sys
from typing import Callable

REPO = pathlib.Path(__file__).resolve().parent.parent
BASELINE = REPO / "tools" / "docs_style_baseline.json"
AVOID = REPO / "CONTEXT-AVOID.md"
V1 = "docs/spec/v1.md"
# The one-line banner every annex carries directly under its title, and no other page
# carries (#2385, ruling 5). A page whose annex scope is narrower says so after it.
BANNER = "> **Normative annex of [Tracer v1](../spec/v1.md#3-wire-format), per v1 §3.**"
MAX_LINE = 100
#: The sidebar's sections, in reader-intent order (#2374, #2381).
SIDEBAR = ("Start here", "Learn", "Concepts", "Recipes", "Reference", "Specification",
           "Decisions", "Evidence")
CAPTION = re.compile(r"^:caption:\s*(.+?)\s*$")

# Generated pages: the generators own their text, and the committed copies are snapshots.
GENERATED = {"docs/performance.md", "docs/test-report.md"}

CONCEPT_HEADINGS = (
    ("In one paragraph", True), ("The picture", True), ("When you need it", False),
    ("How it works", True), ("Worked example", False), ("Cost", True),
    ("What the library does not do", False), ("Pitfalls", False),
    ("Where the rules live", True), ("History", False),
)

OWN_SITES = re.compile(r"https?://(?:libtracer\.avatarsd\.com|avatarsd-llc\.github\.io/libtracer)/\S*")
OWN_REPO_DOC = re.compile(r"https?://github\.com/avatarsd-llc/libtracer/(?:blob|tree)/[^/\s)]+/([^\s)#]+)")
LINK = re.compile(r"\]\(([^)\s]*)\)")
KEYWORD = re.compile(r"\b(?:MUST|SHALL|SHOULD|REQUIRED)\b")
STYLED = re.compile(r"^\s*(style|classDef|linkStyle)\s")  # page-local styling in a diagram
FENCE = re.compile(r"^\s*(`{3,}|~{3,}|:{3,})\s*(\{[\w-]+\})?\s*(\w+)?")


@dataclasses.dataclass(frozen=True)
class Hit:
    path: str
    line: int
    rule: str
    message: str


@dataclasses.dataclass(frozen=True)
class Context:
    """What a page is linted against: the repository, reduced to four questions."""
    annexes: frozenset
    published: Callable[[str], bool]
    exists: Callable[[str], bool]
    avoid: tuple


def annexes_from_v1(v1_text: str) -> set[str]:
    """@brief The annex pages: the bold reference-page bullets of v1 §3, and only those."""
    sec = re.search(r"^## 3\. .*?(?=^## \d)", v1_text, re.M | re.S)
    found = re.findall(r"^- \*\*\[[^\]]*\]\(\.\./reference/([^)#]+)\)\*\*", sec.group(0) if sec else "", re.M)
    return {f"docs/reference/{name}" for name in found}


def parse_avoid(text: str) -> tuple:
    """@brief `- "phrase" → use instead` bullets, in file order."""
    return tuple(re.findall(r'^- "([^"]+)"\s*→\s*(.+?)\s*$', text, re.M))


def _strip_code_spans(line: str) -> str:
    return re.sub(r"`+[^`]*`+", "", line)


def _resolve(page: str, target: str) -> str:
    return posixpath.normpath(posixpath.join(posixpath.dirname(page), target.split("#")[0]))


def _field(lines: list[str], i: int) -> str:
    """The field starting at line i, through the end of its paragraph."""
    out = []
    for ln in lines[i:]:
        if not ln.strip():
            break
        out.append(ln)
    return " ".join(out)


def lint_page(rel: str, text: str, ctx: Context) -> list[Hit]:
    """@brief Every hit on one page, in line order."""
    lines = text.splitlines()
    if rel in ctx.annexes:  # hands off, but for the banner under its title
        first = next((ln for ln in lines[1:] if ln.strip()), "")
        return [] if first.startswith(BANNER) else [
            Hit(rel, 1, "annex-banner", "an annex per v1 §3 opens with the annex banner")]
    if rel == V1 or rel.startswith("docs/spec/rfcs/"):
        return []  # hands off: the spec is changed by erratum or amendment, never by format work
    hits: list[Hit] = []
    if rel == "index.md":
        captions = [(n, m.group(1)) for n, ln in enumerate(lines, 1) if (m := CAPTION.match(ln))]
        if tuple(c for _, c in captions) != SIDEBAR:
            hits.append(Hit(rel, captions[0][0] if captions else 1, "sidebar-order",
                            f"sidebar sections {[c for _, c in captions]} are not {list(SIDEBAR)}"))
    concept = rel.startswith("docs/concepts/") and rel != "docs/concepts/index.md"  # navigation
    headings: list[tuple[int, str]] = []
    cost_parts: list[str] = []
    fence = None  # (marker, directive, body lines, start, is a Mermaid diagram)
    for n, line in enumerate(lines, 1):
        hit = lambda rule, msg: hits.append(Hit(rel, n, rule, msg))  # noqa: E731
        m = FENCE.match(line)
        if fence:
            if line.strip().startswith(fence[0]) and line.strip().strip(fence[0][0]) == "":
                if fence[1] == "{mermaid}" and not ("accTitle:" in fence[2] and "accDescr" in fence[2]):
                    hits.append(Hit(rel, fence[3], "alt-text", "mermaid diagram lacks accTitle: and accDescr"))
                if fence[1] in ("{image}", "{figure}") and ":alt:" not in fence[2]:
                    hits.append(Hit(rel, fence[3], "alt-text", f"{fence[1]} lacks :alt:"))
                if fence[4]:
                    hits.extend(Hit(rel, fence[3] + k, "diagram-style",
                                    "a diagram styles a node on the page; use a vocabulary class")
                                for k, ln in enumerate(fence[2].splitlines(), 1) if STYLED.match(ln))
                fence = None
            else:
                fence = (fence[0], fence[1], fence[2] + line + "\n", fence[3], fence[4])
            continue
        if m:
            fence = (m.group(1), m.group(2), "", n,
                     m.group(2) == "{mermaid}" or (m.group(2) is None and m.group(3) == "mermaid"))
            continue
        prose = _strip_code_spans(line)
        if BANNER in line:
            hit("annex-banner", "only an annex v1 §3 lists carries the annex banner")
        if re.search(r"!\[\s*\]\(", prose):
            hit("alt-text", "image has empty alt text")
        if line.startswith("## "):
            headings.append((n, line[3:].strip()))
        elif line.startswith("### ") and headings and headings[-1][1] == "Cost":
            cost_parts.append(line[4:].strip())
        field = re.match(r"\W*(Normative source|Exercised by)\s*:", prose)
        if field:
            value = _field(lines, n - 1)
            targets = LINK.findall(_strip_code_spans(value))
            if field.group(1) == "Normative source":
                bad = [t for t in targets if t.startswith("http") or
                       (_resolve(rel, t) != V1 and _resolve(rel, t) not in ctx.annexes)]
                if bad or not targets:
                    hit("normative-source", "names only v1 or an annex, never "
                        + (", ".join(bad) if bad else "nothing"))
            else:
                names = re.findall(r"`([^`]+)`", value) + [_resolve(rel, t) for t in targets]
                missing = [x for x in names if not ctx.exists(x)]
                if missing or not names:
                    hit("exercised-by", "names no existing vector or test: "
                        + (", ".join(missing) if missing else "(none named)"))
        if (len(LINK.sub("]()", line)) > MAX_LINE and not line.lstrip().startswith(("|", "<"))
                and not re.match(r"\s*\[[^\]]+\]:\s", line)):
            hit("line-length", f"prose line over {MAX_LINE} characters")
        for url in OWN_SITES.findall(prose):
            hit("own-site-link", f"link our own page relatively, not {url.rstrip(')')}")
        for path in OWN_REPO_DOC.findall(prose):
            if ctx.published(path):
                hit("own-site-link", f"{path} is published; link it relatively")
        if KEYWORD.search(prose):
            hit("restatement", "an RFC 2119 keyword outside the spec; link the spec clause instead")
        low = prose.lower()
        for phrase, instead in ctx.avoid:
            if phrase.lower() in low:
                hit("avoid-phrase", f'"{phrase}": say {instead}')
    if concept:
        hits += _concept_hits(rel, headings)
        cost = next((n for n, h in headings if h == "Cost"), None)
        if cost is not None and "Latency" not in cost_parts:
            hits.append(Hit(rel, cost, "concept-sections",
                            'Cost has no "### Latency"; say "not benched" and name the bench'))
    return sorted(hits, key=lambda h: h.line)


def _concept_hits(rel: str, headings: list[tuple[int, str]]) -> list[Hit]:
    order = [name for name, _ in CONCEPT_HEADINGS]
    hits = [Hit(rel, n, "concept-sections", f'"{h}" is not a template heading')
            for n, h in headings if h not in order]
    seen = [(n, h) for n, h in headings if h in order]
    hits += [Hit(rel, 1, "concept-sections", f'required section "{name}" is missing')
             for name, required in CONCEPT_HEADINGS if required and name not in {h for _, h in seen}]
    last = -1
    for n, h in seen:
        if order.index(h) < last:
            hits.append(Hit(rel, n, "concept-sections", f'"{h}" is out of template order'))
            break
        last = order.index(h)
    return hits


def new_hits(hits: list[Hit], baseline: dict) -> list[tuple[str, str, int, int]]:
    """@brief (page, rule, count, baseline count) for every count above its baseline."""
    counts: dict[tuple[str, str], int] = {}
    for h in hits:
        counts[(h.path, h.rule)] = counts.get((h.path, h.rule), 0) + 1
    return [(p, r, c, baseline.get(p, {}).get(r, 0)) for (p, r), c in sorted(counts.items())
            if c > baseline.get(p, {}).get(r, 0)]


def baseline_json(hits: list[Hit]) -> str:
    out: dict[str, dict[str, int]] = {}
    for h in hits:
        out.setdefault(h.path, {}).setdefault(h.rule, 0)
        out[h.path][h.rule] += 1
    return json.dumps(out, indent=1, sort_keys=True) + "\n"


def _conf_patterns() -> tuple[list[str], list[str]]:
    tree = ast.parse((REPO / "docs" / "conf.py").read_text())
    found = {t.id: ast.literal_eval(node.value) for node in tree.body if isinstance(node, ast.Assign)
             for t in node.targets if isinstance(t, ast.Name) and t.id in ("include_patterns", "exclude_patterns")}
    return found.get("include_patterns", []), found.get("exclude_patterns", [])


def real_context() -> tuple[Context, list[str]]:
    """@brief The real repository as a Context, and the pages to lint."""
    files = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True,
                           check=True).stdout.splitlines()
    inc, exc = _conf_patterns()
    published = lambda p: (any(fnmatch.fnmatch(p, g) for g in inc)  # noqa: E731
                           and not any(fnmatch.fnmatch(p, g) for g in exc))
    names = set(files)
    for f in files:
        parts = f.split("/")
        if f.startswith("tests/conformance/vectors/"):  # a vector, by its directory or its name
            names.update((posixpath.dirname(f), parts[-2]))
        if "tests" in parts:  # a test, by its file stem
            names.add(pathlib.PurePosixPath(f).stem)
    ctx = Context(annexes=frozenset(annexes_from_v1((REPO / V1).read_text())), published=published,
                  exists=names.__contains__, avoid=parse_avoid(AVOID.read_text()))
    pages = [f for f in files if f.endswith(".md") and (f.startswith("docs/") or f == "index.md")
             and f not in GENERATED and "/_" not in f]
    return ctx, pages


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gate", action="store_true", help="exit 1 when a count exceeds the baseline")
    ap.add_argument("--baseline", default=str(BASELINE))
    ap.add_argument("--update-baseline", action="store_true", help="record today's hits")
    args = ap.parse_args(argv)
    ctx, pages = real_context()
    hits = [h for p in pages for h in lint_page(p, (REPO / p).read_text(), ctx)]
    if args.update_baseline:
        pathlib.Path(args.baseline).write_text(baseline_json(hits))
        print(f"docs style: baseline recorded, {len(hits)} hits")
        return 0
    path = pathlib.Path(args.baseline)
    baseline = json.loads(path.read_text()) if path.exists() else {}
    fresh = new_hits(hits, baseline)
    fresh_keys = {(p, r) for p, r, _, _ in fresh}
    for h in hits:
        mark = "NEW " if (h.path, h.rule) in fresh_keys else ""
        print(f"{mark}{h.path}:{h.line}: [{h.rule}] {h.message}")
    for p, r, c, b in fresh:
        print(f"NEW {p}: [{r}] {c} hits, baseline {b}")
    mode = "gating" if args.gate else "report-only"
    print(f"docs style ({mode}): {len(hits)} hits on {len(pages)} pages, "
          f"{len(fresh)} page/rule counts above the baseline")
    return 1 if args.gate and fresh else 0


if __name__ == "__main__":
    sys.exit(main())
