# Build configuration — reference-implementation design notes

> **Status:** design (descriptive record of shipped behaviour, not a proposal).
> **Target:** the C++23 reference implementation under [`../../../core/`](../../../core/).
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
| [`00-configuration-space.md`](00-configuration-space.md) | The whole space: the three kinds of axis and why there is no fourth; the generated-header delivery mechanism and who actually chooses the values; the module set; the five sized/bound knobs with measured per-target costs; the four per-target behaviours that surprise people; what is deliberately *not* configurable and why; how to add an axis without tripping the render-site gap. |

```{toctree}
:hidden:
:maxdepth: 1

The configuration space <00-configuration-space>
```
