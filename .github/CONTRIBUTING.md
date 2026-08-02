# Contributing to libtracer

Thanks for your interest. There are several distinct ways to contribute, and they have different processes.

## Quick map

| You want to...                                | Read                                            |
|-----------------------------------------------|-------------------------------------------------|
| Fix a bug or improve the reference impl       | "Code contributions" below                      |
| Propose a wire-format / spec change           | [GOVERNANCE.md](GOVERNANCE.md) — RFC process    |
| Add a new transport binding or integration    | "New integrations" below                        |
| Register an independent implementation        | [docs/implementations.md](../docs/implementations.md)   |
| Build a bridge to a closed protocol           | "Bridges" below                                 |
| Improve docs                                  | Just open a PR                                  |

## Developer Certificate of Origin

Contributions are accepted under the [Developer Certificate of Origin](https://developercertificate.org/). You certify the DCO by signing your commits:

```sh
git commit -s -m "your message"
```

This adds a `Signed-off-by:` line. Unsigned commits will be asked to be amended.

We use DCO rather than a CLA to keep the contribution barrier low. The maintainers reserve the right to introduce a CLA later if dual-licensing or relicensing becomes necessary; existing DCO contributions remain valid under Apache 2.0 in any case.

## Code contributions

1. Open an issue first if the change is non-trivial. Saves wasted work if the direction is wrong.
2. Fork, branch, push, open a PR.
3. Sign commits (`-s`).
4. CI must be green.
5. For C/C++ changes, run `clang-format` (config in `.clang-format`).
6. Public API changes require a note in the relevant `CHANGELOG.md`.

Cutting a release (version bump, tag, registry publishes) follows [RELEASING.md](RELEASING.md).

## New integrations

A new integration (a new platform, transport, or framework wrapper) goes under `integrations/<name>/`. Include:

- A `README.md` explaining what it integrates with and how to use it.
- A minimal example under `examples/<name>/`.
- A test that runs in CI, even if it's just a build check.

Integrations may be maintained by sub-maintainers separate from the core team. List them in the integration's `README.md`.

## Bridges (smart-device adapters for incompatible protocols)

Bridges are first-class citizens — translating Modbus, Z-Wave, proprietary vendor X, etc. into libtracer is exactly what this project exists to enable. A bridge can be:

- **In-tree** — under `integrations/bridges/<protocol>/`. Use this if the bridge has no external dependencies that complicate the core build.
- **Out-of-tree** — your own repo, registered in [docs/implementations.md](../docs/implementations.md). Use this if the bridge depends on a proprietary SDK or has its own release cadence.

Either way, add a row to the registry so users can find it.

## Spec changes

See [GOVERNANCE.md](GOVERNANCE.md) for the RFC process — it is the authority; this is only a pointer.

tl;dr: pick the instrument first. A correction to a normative document that contradicts **already-shipped, already-agreed** behaviour is an **erratum** — an ordinary PR, no comment window, and it may not change the wire surface. A change to the normative surface itself is an **amendment** — open an `rfc`-tagged issue, then a PR under `docs/spec/rfcs/`, and address feedback from registered implementers. The amendment comment window is **at least 14 days when it runs**, but it is **waived by default** while the project is solo-maintained and is invoked explicitly when outside input is wanted; do not defer a correction for a window that is not running. See [Errata, amendments, and the comment window](GOVERNANCE.md#errata-amendments-and-the-comment-window).

## Code of conduct

Be kind. Assume good faith. We don't have a formal CoC document yet; if a situation arises that needs one, email the maintainers and we'll adopt the [Contributor Covenant](https://www.contributor-covenant.org/).
