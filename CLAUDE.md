# libtracer

libtracer is a spec-first protocol project. See [GOVERNANCE.md](.github/GOVERNANCE.md) for the three decision domains (spec / reference impl / tooling) and the RFC process for spec changes.

## Working in this repo

- **Every change lands via a pull request.** No direct pushes to `main` — branch, push, open a PR, merge through GitHub.
- **Sign commits** with `-s` (DCO required per [CONTRIBUTING.md](.github/CONTRIBUTING.md)). Unsigned commits will be asked to be amended.
- **Do not add `Co-Authored-By` trailers** to commit messages.
- **C/C++ changes** must pass `clang-format-18` (config at `.clang-format`). CI runs 18.1.3; a bare `clang-format` of another major version reports unrelated files as dirty. Every CI gate and a before-you-push list are in [the runbook](docs/agents/pr-review.md#the-runbook-to-hand-the-contributor).
- **C++ naming, namespaces and comments:** follow [core/STYLE.md](core/STYLE.md), the one statement of those rules. Read it before writing C++; public headers are CI-gated by `core/Doxyfile` (`WARN_AS_ERROR`).
- **Spec changes** — anything under [docs/spec/](docs/spec/), **plus every reference page that [docs/spec/v1.md §3](docs/spec/v1.md) incorporates as a normative annex** (that list is the only source; it is restated at [docs/spec/index.md §What is normative](docs/spec/index.md)) — require an RFC under [docs/spec/rfcs/](docs/spec/rfcs/) per [GOVERNANCE.md](.github/GOVERNANCE.md). Pick the right instrument: an **erratum** (the text contradicts shipped, already-agreed behaviour — ordinary PR, no window, must not change the wire surface) or an **amendment** (the normative surface itself changes — RFC + maintainer approval, 14-day window **waived by default** while solo-maintained, invoked explicitly when outside input is wanted). Do not defer a correction for a window that is waived.
- **Public API changes** require a note in the relevant `CHANGELOG.md`.

## Design rules (standing)

- **No timers or clock reads in libtracer.** A deadline is driven by the application through a seam that takes `now` from the app's own clock; without it, an operation resolves on its other events (change, link down, teardown).
- **Lower complexity by deleting, normalizing or merging branches, never by splitting a function into helpers**, which only moves them. Judge a refactor by the total cyclomatic complexity of the file or path, not by one function's score.
- **Compile-time by default, receiver pays, no library-internal buffers:** what a build can decide is a compile-time policy, and a peer-provoked cost is drawn from the receiving link's own source.

## Where to look

| You need | Read |
| --- | --- |
| Domain vocabulary — before naming any concept | [CONTEXT.md](CONTEXT.md) |
| The wire protocol (normative) | [docs/spec/v1.md](docs/spec/v1.md) and the annexes its §3 incorporates |
| Which pages are normative | [docs/spec/v1.md §3](docs/spec/v1.md), restated at [docs/spec/index.md](docs/spec/index.md) |
| What the system is: layers, load-bearing claims | [docs/reference/00-overview.md](docs/reference/00-overview.md), then [docs/reference/](docs/reference/) |
| Why it looks the way it does | [docs/adr/](docs/adr/) and git history |
| Spec-change proposals and their history | [docs/spec/rfcs/](docs/spec/rfcs/) |
| C++ naming, comments, layers | [core/STYLE.md](core/STYLE.md) |
| Decision domains, erratum vs amendment | [.github/GOVERNANCE.md](.github/GOVERNANCE.md) |
| Review criteria, CI gates, before-you-push list | [docs/agents/pr-review.md](docs/agents/pr-review.md) |

**Precedence:** the spec (`v1.md` and its annexes) wins over every other doc, `docs/reference/` wins over planning docs, and ADRs carry the rationale, not the rule. If your output contradicts an ADR or a load-bearing reference page, say so explicitly rather than overriding it silently.

## Agent skills

This repo is set up for use with Matt Pocock's engineering skills (`triage`, `to-issues`, `to-prd`, `improve-codebase-architecture`, `diagnose`, `tdd`, `grill-with-docs`, etc.).

### PR review

An automated first-pass review runs from a cloud routine on the maintainer's Claude account when a pull request gets the `reviewable` label, as [docs/agents/pr-review.md](docs/agents/pr-review.md) defines; a push alone does not trigger it. The review is judged against that file: the same rubric a maintainer should use by hand, and the one a contributor can run on themselves by naming it (`Review my branch against docs/agents/pr-review.md`). The routine reads that file from the checkout, so **the review criteria are edited there, in git, and not in the routine's prompt.**

Its approval is the floor of review, not the ceiling. Two things it will not do: it never comments on an issue (the tracker is human-owned, and its surface is the pull request), and it never puts a suspected vulnerability in a public comment — that goes to a private advisory per [SECURITY.md](SECURITY.md).

### Issue tracker

Issues live in **GitHub Issues at [`avatarsd-llc/libtracer`](https://github.com/avatarsd-llc/libtracer/issues)**. Use the `gh` CLI for all operations:

- Create: `gh issue create --title "..." --body "..."`
- Read: `gh issue view <number> --comments`
- List: `gh issue list --state open --label <label>`
- Comment: `gh issue comment <number> --body "..."`
- Label: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- Close: `gh issue close <number> --comment "..."`

When a skill says "publish to the issue tracker," create a GitHub issue. When it says "fetch the relevant ticket," run `gh issue view <number> --comments`. Spec-change discussions use issues tagged `rfc` per [GOVERNANCE.md](.github/GOVERNANCE.md).

### Project board

All open issues are tracked on the org project **[libtracer roadmap](https://github.com/orgs/avatarsd-llc/projects/4)**, which auto-adds every new issue. Keep it current as part of the work, not as a separate chore:

- **Milestone = release.** Each planned issue carries the milestone of the release it ships in (`v0.18.0`, `v0.19.0`, …). No milestone means unscheduled (horizon/roadmap items).
- **Status** (project field):
  - `Backlog`: not startable: it has an open blocker, or is labelled `needs-triage` / `needs-info` / `needs-rfc` / `blocked`.
  - `Ready`: `ready-for-agent` or `ready-for-human`, with every blocker closed.
  - `In progress`: someone (or an agent) is working on it.
  - `In review`: an open PR addresses it.
  - `Done`: closed.
  When an issue closes, re-check the issues it blocked and move any that are now unblocked from `Backlog` to `Ready`.
- **Priority** is an org issue field (`Urgent` = blocks the release or the train in flight; `High` = planned for the next release; `Medium` = the release after; `Low` = unscheduled). Set it with `gh api -X POST repos/avatarsd-llc/libtracer/issues/<n>/issue-field-values` and a body of `{"issue_field_values":[{"field_id":20337321,"value":"High"}]}`.
- **Size** (project field, `XS`–`XL`): set it when filing.
- The **Iteration** field is unused.

Every new issue gets a milestone (or deliberately none), a priority, and a status when it is filed.

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

When exploring or producing output, read the glossary first, then the overview, then the spec ([Where to look](#where-to-look)).
