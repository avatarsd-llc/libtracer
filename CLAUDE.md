# libtracer

libtracer is a spec-first protocol project. See [GOVERNANCE.md](GOVERNANCE.md) for the three decision domains (spec / reference impl / tooling) and the RFC process for spec changes.

## Working in this repo

- **Every change lands via a pull request.** No direct pushes to `main` — branch, push, open a PR, merge through GitHub.
- **Sign commits** with `-s` (DCO required per [CONTRIBUTING.md](CONTRIBUTING.md)). Unsigned commits will be asked to be amended.
- **Do not add `Co-Authored-By` trailers** to commit messages.
- **C/C++ changes** must pass `clang-format` (config at `.clang-format`).
- **Spec changes** (anything under [docs/spec/](docs/spec/)) require an RFC under [docs/spec/rfcs/](docs/spec/) per [GOVERNANCE.md](GOVERNANCE.md). A 14-day comment window applies before acceptance.
- **Public API changes** require a note in the relevant `CHANGELOG.md`.

## Where the canonical material lives

- **Normative spec:** [docs/spec/v1.md](docs/spec/v1.md) — the wire protocol; immutable once released.
- **Reference (descriptive):** [docs/reference/](docs/reference/) — start at [00-overview.md](docs/reference/00-overview.md). The "what it is" alongside the normative spec. When reference and planning docs disagree, reference wins.
- **Design rationale and history:** [docs/plans/](docs/plans/) — predates the current spec; explains "why it looks the way it does."
- **Glossary (de-facto domain vocabulary):** [docs/plans/99-glossary.md](docs/plans/99-glossary.md) — read this before naming any domain concept (vertex, edge, path, view, segment, TLV, bridge, address-shift slicing, etc.).
- **Implementation analysis:** [docs/analisys/](docs/analisys/) — five-cycle iterative reviews and the decisions they raise.

## Repo layout

| Path                     | Purpose                                                   |
| ------------------------ | --------------------------------------------------------- |
| [core/](core/)           | C++ reference implementation (CMake)                      |
| [bindings/](bindings/)   | Language bindings — currently `rust/`, `typescript/`      |
| [integrations/](integrations/) | Platform integrations — `arduino/`, `esphome/`, `platformio/` |
| [implementations/](implementations/) | Registry of independent third-party implementations |
| [examples/](examples/)   | Per-platform usage examples                               |
| [tests/](tests/)         | Conformance test vectors                                  |
| [docs/](docs/)           | Spec, reference, plans, analysis (see above)              |

## Agent skills

This repo is set up for use with Matt Pocock's engineering skills (`triage`, `to-issues`, `to-prd`, `improve-codebase-architecture`, `diagnose`, `tdd`, `grill-with-docs`, etc.).

### Issue tracker

Issues live in **GitHub Issues at [`avatarsd-llc/libtracer`](https://github.com/avatarsd-llc/libtracer/issues)**. Use the `gh` CLI for all operations:

- Create: `gh issue create --title "..." --body "..."`
- Read: `gh issue view <number> --comments`
- List: `gh issue list --state open --label <label>`
- Comment: `gh issue comment <number> --body "..."`
- Label: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- Close: `gh issue close <number> --comment "..."`

When a skill says "publish to the issue tracker," create a GitHub issue. When it says "fetch the relevant ticket," run `gh issue view <number> --comments`. Spec-change discussions use issues tagged `rfc` per [GOVERNANCE.md](GOVERNANCE.md).

### Triage labels

The five canonical triage roles map 1:1 to label strings in this repo (no remapping):

| Role               | Meaning                                              |
| ------------------ | ---------------------------------------------------- |
| `needs-triage`     | Maintainer needs to evaluate this issue              |
| `needs-info`       | Waiting on reporter for more information             |
| `ready-for-agent`  | Fully specified, ready for an AFK agent to pick up   |
| `ready-for-human`  | Requires human implementation                        |
| `wontfix`          | Will not be actioned                                 |

When a skill mentions a triage role, apply the matching label string. Labels will be created on first use by `gh label create` if they don't yet exist.

### Domain docs

**Multi-context layout.** No formal `CONTEXT.md` / `CONTEXT-MAP.md` exists yet — they'll be created lazily by `/grill-with-docs` as per-binding / per-integration vocabulary actually diverges from the project-wide glossary.

Until then, when exploring or producing output:

1. Read **[docs/plans/99-glossary.md](docs/plans/99-glossary.md)** first — it's the de-facto root glossary. Use its vocabulary; don't drift to synonyms.
2. Read **[docs/reference/00-overview.md](docs/reference/00-overview.md)** for the six-layer model and load-bearing architectural commitments.
3. Read **[docs/spec/v1.md](docs/spec/v1.md)** for normative behavior. When the spec and any other doc disagree, the spec wins.
4. ADRs (when written) will live under `docs/adr/`. Not yet present — proceed silently if absent. Until then, design rationale lives in [docs/plans/](docs/plans/).

If your output contradicts an existing ADR or a load-bearing reference doc, surface the contradiction explicitly rather than silently overriding.
