---
name: docs-article
description: Write, extend or restructure a page of the libtracer docs site in the unified format (#2374): pick the page type, follow the concept-page template, draw diagrams in the shared visual vocabulary, link the spec instead of restating it, and run the style lint. Use for a new docs page, a concept page, a diagram, a rewrite of a reference or guide page, or before asking for review of any docs change, whether a person or an agent is writing.
---

# Writing a docs page

The docs site has one format (spec #2374, rulings of 2026-10-29). People and agents use
this same procedure, and a page that follows it looks the same whoever wrote it.

Before naming anything, read [CONTEXT.md](../../../CONTEXT.md), the glossary.
[CONTEXT-AVOID.md](../../../CONTEXT-AVOID.md) beside it lists the phrases the lint
reports. Read [docs/spec/v1.md §3](../../../docs/spec/v1.md) to see which pages are
normative.

## Two rules that come first

- **The spec holds the rules.** A guide, concept or reference page links to the clause
  in v1 or an annex and never restates it. Do not paraphrase a MUST into a softer
  sentence, and do not quote it either. Write what the rule is *for* and link where it
  lives. The lint's `restatement` rule reports an RFC 2119 keyword stated outside the
  spec.
- **Hands off the annexes.** The pages v1 §3 incorporates (v1 §3 is the only list) are
  never edited by format work: no rewording, no moved content, no new diagrams.
  A contradiction you find is its own erratum, under `.github/GOVERNANCE.md`. The lint
  does not touch them, and neither do you. Each annex opens with the one-line
  "Normative annex of Tracer v1, per v1 §3" banner under its title (#2385), and no other
  page carries it; the lint's `annex-banner` rule holds both halves.

## Pick the page type before you write

Each page does one job. When a draft is doing two, split it into two pages.

| Type | Job | Where |
| --- | --- | --- |
| tutorial | a first success, step by step | Start here, Learn |
| how-to | one task, with a compile-tested example | Recipes (`docs/examples/`) |
| concept | why and how one idea works | Concepts (`docs/concepts/`) |
| reference | the facts, numbered pages, no renames | `docs/reference/` |
| API | module pages and the generated API | `docs/modules/` |
| spec | the rules: v1 and its annexes, which win | `docs/spec/` |
| RFC, ADR | the history of a rule or a decision | `docs/spec/rfcs/`, `docs/adr/` |
| evidence | generated performance and test reports | never hand-edited |

The sidebar runs by reader intent: Start here (with an MCU lane), Learn, Concepts,
Recipes, Reference, Specification, Decisions, Evidence. Set the order through toctree
order and never rename a file. A new section (Concepts, Start here) goes into
`include_patterns` in `docs/conf.py` in the same PR as its first page.

Extending an existing page beats adding a new one. A concept page links into reference
sections and does not absorb them. It may replace the *explanation* on a non-normative
page with a link, but sizing and archetype references stay whole.

## The concept-page template

A page under `docs/concepts/` uses these level-2 headings, spelled exactly like this and
in this order. Required ones are marked R. An optional one may be left out, but the rest
keep their order. The lint's `concept-sections` rule checks presence and order, and
reports any heading of the page's own.

1. `## In one paragraph` (R): what it is and why it exists, in one paragraph.
2. `## The picture` (R): one diagram, one zoom level, at most 12 nodes.
3. `## When you need it`
4. `## How it works` (R)
5. `## Worked example`: real bytes, generated from a conformance vector, never typed.
   Use the `{hex-walk}` directive (`docs/_ext/hexwalk.py`): name the vector and label its
   leaves in order, one `<format>: <label>` line each; `docs/concepts/batching.md` shows it.
6. `## Cost` (R): wire bytes, allocations, latency, and flash/RAM per NARROW/MID/WIDE
   target, including rows above 1 KiB. Numbers come from the size gate or the benches.
7. `## What the library does not do`: for example pacing, timers or playout.
8. `## Pitfalls`: symptom, then cause, then fix.
9. `## Where the rules live` (R): carries the `**Normative source:**` field.
10. `## History`: collapsed into a dropdown. RFCs are named here and only here.

Budgets: 150 to 350 source lines, at most 2,000 words of prose, at most 4 diagrams.
End the page with "Next concept" and "Go deeper" links (explanation, then reference,
then spec). A large concept gets a parent page plus child pages, each following the
template.

The template freezes only after the two pilots (batching, then transports) and the
maintainer's approval (#2379, the template freeze). Until then, a page that needs a
different shape is evidence for the pilots: say so in the PR and do not quietly
diverge.

### The two header fields

- `**Normative source:**` names **only** v1 or an annex anchor, as relative links
  (`[v1 §3](../spec/v1.md#3-wire-format)`). It never names an RFC, because RFCs are
  history. This is rule `normative-source`.
- `**Exercised by:**` names the conformance vector (`` `batch-time-base` `` or its
  directory under `tests/conformance/vectors/v1/`) or the test (by file stem) that
  proves the claim, and each one must exist. This is rule `exercised-by`.

## Diagrams: one visual vocabulary

Shape carries the meaning and colour only reinforces it, so a diagram still reads in
greyscale.

| Thing | Shape (Mermaid flowchart) |
| --- | --- |
| frame | parallelogram `[/frame/]` |
| TLV / record | rectangle `[record]` |
| vertex | rounded box `(vertex)` |
| ring | cylinder `[(ring)]` |
| application | stadium `([application])` |
| peer | subroutine `[[peer]]` |
| not the library | grey, dashed |
| a choice on one thing | rhombus `{choice}` with labelled edges |

- An operation (compose, swap, propagate, FOLD) is an **edge label**, never a box.
- A link is a thick edge labelled with its transport. A control or reply path is a dotted
  edge.
- MCU and host nodes go in dashed or solid groups titled with the target.
- Colours are the Okabe-Ito classes, defined once in `docs/_static/diagram-vocabulary.css`
  with paired light and dark values: `frame`, `record`, `vertex`, `ring`, `app`, `peer`
  and `notlib`. Put one on a node with `:::vertex` or a `class A,B vertex` line. Never
  style a node on the page (`style`, `classDef`, `linkStyle`). Use at most four classes
  per diagram, and give every classed diagram a one-line legend in its `:caption:`. The
  smoke page `docs/style/diagram-vocabulary.md` draws every class; check it in both themes
  after a palette or Mermaid change, and run `python3 tools/check_diagram_contrast.py`.
- Pick the diagram kind by its job:

  | Job | Kind |
  | --- | --- |
  | structure, flow | flowchart |
  | an ordered exchange | sequence, one per mode |
  | a lifecycle | state |
  | a fixed-width header | packet diagram, and only for a fixed-width header |
  | a variable-length layout | generated hex walk with per-field dropdowns |
  | a big-picture figure | hand-made SVG with committed source, at most 3 site-wide |

- **Alt text on every diagram.** Inside a `{mermaid}` directive, give `accTitle:` and
  `accDescr:` lines. An image gets alt text, and `{image}`/`{figure}` gets `:alt:`. This
  is rule `alt-text`.
- New site pages use the directive spelling (```` ```{mermaid} ````). Bare
  ```` ```mermaid ```` fences are kept so GitHub renders them, and the lint skips them.
- No heavy or networked tool: no Kroki, LaTeX, mermaid-cli or D2. Mermaid is pinned and
  served from the site's own static assets, not a CDN (ruling 1): the pin and the upgrade
  steps are in `docs/conf.py`, the files under `docs/_static/vendor/`.

## Mechanics the lint checks

- Prose lines are at most 100 characters. Link targets are not counted, and table rows,
  code and HTML are exempt (`line-length`).
- Link our own published pages relatively, never as `libtracer.avatarsd.com`, the
  GitHub Pages URL or a GitHub `blob/` URL. A GitHub link is right only for a file the
  site does not publish (`own-site-link`).
- Every code sample compiles through the `docs/examples/` path. Every hex dump comes
  from a conformance vector.
- Terms come from the glossary. Do not mint a second name for an existing concept.

## Before you ask for review

Run these from the repo root. The lint is report-only for now, so read its output
for your own pages: a line marked `NEW` is a hit your change added.

```sh
python3 tools/check_docs_style.py                      # style lint, report-only
python3 tools/check_doc_citations.py                   # symbol citations resolve
python3 tools/gen_file_map.py --check                  # file map is current
sphinx-build -n -W --keep-going -b html -c docs . docs/_build/html
```

`python3 tools/check_docs_style.py --update-baseline` records today's hits. Run it only
when a change deliberately moves the baseline, such as fixing hits or adding a rule, and
say so in the PR. The style pass (#2373) switches the docs workflow to `--gate`. The rule
against per-page diagram styling stays off until the CSS vocabulary is proven in the
batching pilot (#2372).
