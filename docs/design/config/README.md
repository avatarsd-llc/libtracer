# Build configuration — reference-implementation design notes

> **Scope:** the build configuration of the C++23 reference implementation. Not normative.
> **Target:** the sources under [`core/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/).
> **Companions:** [`../../reference/10-module-catalog.md`](../../reference/10-module-catalog.md)
> says what each module is *responsible for*, at standard level;
> [`../../reference/12-deployment-profiles.md`](../../reference/12-deployment-profiles.md) says
> which combinations serve which deployments. Neither is allowed to name a CMake option, and
> that is the gap these notes fill. Nothing here is normative.

The reference suite is deliberately not a build guide, so a reader who wants to know *which
knobs exist, what each costs on their target, and which constants are off-limits* has nowhere
in it to look. That question is entirely implementation-specific — a second implementer needs
none of it — but an integrator needs all of it.

| File | Topic |
| --- | --- |
| [`00-configuration-space.md`](00-configuration-space.md) | Every configuration axis the build exposes, its per-target cost, and the constants that are not axes. |

```{toctree}
:hidden:
:maxdepth: 1

The configuration space <00-configuration-space>
```
