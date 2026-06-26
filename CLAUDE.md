# libtracer

libtracer is a spec-first protocol project. See [GOVERNANCE.md](GOVERNANCE.md) for the three decision domains (spec / reference impl / tooling) and the RFC process for spec changes.

## Working in this repo

- **Every change lands via a pull request.** No direct pushes to `main` — branch, push, open a PR, merge through GitHub.
- **Sign commits** with `-s` (DCO required per [CONTRIBUTING.md](CONTRIBUTING.md)). Unsigned commits will be asked to be amended.
- **Do not add `Co-Authored-By` trailers** to commit messages.
- **C/C++ changes** must pass `clang-format` (config at `.clang-format`).
- **C++ naming & comments** (full rules in [core/STYLE.md](core/STYLE.md)): types are `snake_case` with a `_t` suffix (`mem_backend_t`, `view_t`) — **never PascalCase**; enums are `enum class` with `SCREAMING_SNAKE` scoped values (`io_dir_t::DEVICE_TO_CPU`); namespaces mirror the layer model (`tr::mem` L0, `tr::view` L1, `tr::wire` L2/L3, `tr::graph` L4) and never reuse an error-concept word. Doxygen on every public symbol using **`/** … */` block comments only** (never `///`; trailing docs use `/**< … */`), `@brief` mandatory — CI-gated by `core/Doxyfile` (`WARN_AS_ERROR`).
- **Spec changes** (anything under [docs/spec/](docs/spec/)) require an RFC under [docs/spec/rfcs/](docs/spec/) per [GOVERNANCE.md](GOVERNANCE.md). A 14-day comment window applies before acceptance.
- **Public API changes** require a note in the relevant `CHANGELOG.md`.

## Where the canonical material lives

- **Normative spec:** [docs/spec/v1.md](docs/spec/v1.md) — the wire protocol; immutable once released.
- **Reference (descriptive):** [docs/reference/](docs/reference/) — start at [00-overview.md](docs/reference/00-overview.md). The "what it is" alongside the normative spec. When reference and planning docs disagree, reference wins.
- **Design rationale and history:** [docs/adr/](docs/adr/) (ADRs) and git history — explains "why it looks the way it does."
- **Glossary (canonical domain vocabulary):** [CONTEXT.md](CONTEXT.md) — the root context glossary; read this before naming any domain concept (vertex, edge, path, view, segment, TLV, bridge, address-shift slicing, etc.).
- **Decisions & RFCs:** [docs/adr/](docs/adr/) records architecture decisions; [docs/spec/rfcs/](docs/spec/rfcs/) holds spec-change proposals. (The earlier `docs/analisys/` reviews were consolidated into these and removed.)

## Repo layout

| Path                     | Purpose                                                   |
| ------------------------ | --------------------------------------------------------- |
| [core/](core/)           | C++ reference implementation (CMake)                      |
| [bindings/](bindings/)   | Language bindings — currently `rust/`, `typescript/`      |
| [integrations/](integrations/) | Platform integrations — `arduino/`, `esphome/`, `platformio/` |
| [implementations/](implementations/) | Registry of independent third-party implementations |
| [examples/](examples/)   | Per-platform usage examples                               |
| [tests/](tests/)         | Conformance test vectors                                  |
| [docs/](docs/)           | Spec, reference, ADRs, RFCs (see above)              |

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

**Multi-context layout.** A root [CONTEXT.md](CONTEXT.md) holds the canonical project-wide vocabulary. No `CONTEXT-MAP.md` exists yet — per-binding / per-integration context files will be created lazily by `/grill-with-docs` if and when their vocabulary diverges from the root.

Until then, when exploring or producing output:

1. Read **[CONTEXT.md](CONTEXT.md)** first — the canonical root glossary. Use its vocabulary; don't drift to synonyms.
2. Read **[docs/reference/00-overview.md](docs/reference/00-overview.md)** for the six-layer model and load-bearing architectural commitments.
3. Read **[docs/spec/v1.md](docs/spec/v1.md)** for normative behavior. When the spec and any other doc disagree, the spec wins.
4. ADRs live under [docs/adr/](docs/adr/) — read them; `0001`–`0007` record the strawberry-fw extraction and the protocol-v1 wire/API commitments (versioning, retire-LIST, trailer-CRC, fixed-width length, read/write/await, normative-by-incorporation). They also carry the design *rationale* (the "why").

If your output contradicts an existing ADR or a load-bearing reference doc, surface the contradiction explicitly rather than silently overriding.
