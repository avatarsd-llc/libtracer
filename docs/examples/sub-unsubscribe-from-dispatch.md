# Unsubscribing from inside a delivery (L4 graph)

:::{admonition} Applies to the default `reclaim_qsbr_t` only
:class: important

This is the one example whose subject is **policy-dependent**. It demonstrates a shape that
the default `reclaim_qsbr_t` supports and that `reclaim_strict_t` forbids, so it runs its
body under the default binding and **skips** under `reclaim_strict_t`, printing
`skipped: this build forbids unsubscribing from inside a delivery` and passing. The policy is
a build-time type rather than a CMake option ([ADR-0068](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cpp-config-header.md)),
so the target is always built and always linked — it follows the binding with `if constexpr`,
exactly as `core/tests/reclaim_test.cpp` does for its own re-entrant cases. Every other
example on this site applies to both policies.
:::

A subscriber callback that retires its own subscription is the one case where "the edge is
gone" and "the `{fn, ctx}` pair is dead" are not the same moment: the fan-out that invoked
the callback is still walking a snapshot that names that pair. The default `reclaim_qsbr_t`
policy retires the pair and runs its release hook once **every dispatching thread has passed
a quiescent state**. On this single-threaded example that is this thread's own outermost
dispatch exit, before the `write()` that started the delivery hands control back
([ADR-0080](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md),
Amendment 1).

## What to notice

- **Retirement is immediate; release is not.** The example's second write reaches nobody —
  the next snapshot already skips the slot — yet the hook has not run when the callback
  returns. Both halves are asserted, and the run prints the flag as observed *inside* the
  callback.
- **The library decides which grace point applies, and the caller never asks.** With no
  dispatch live the same call releases inline (see [unsubscribe](sub-unsubscribe.md)); with
  one live it defers. The signal is the hook in both cases — there is no poll and no verb the
  embedder must remember.
- **The policy is a build-time type, not a runtime flag.** It is named by
  `default_config_t::reclaim_policy_t` and bound in `libtracer/config_override.hpp`, on the
  same seam pattern as the ACL policy and the LKV slot. The example prints
  `reclaim_policy_t::kName` so a run says which one it exercised.
- **`reclaim_strict_t` forbids this shape, and does not detect it.** It is the zero-cost
  opt-out for NARROW bare metal with no preemption and no second core. It keeps
  no dispatch state, so no build can see a violation. The example branches on
  `kDefersToDispatchExit` with `if constexpr` and skips its body there — the constant is the
  portable way to ask "may I do this on this target?", and asking it is the point. An example
  that does not apply to a binding must skip, never fail to compile, so the branch is a
  runtime skip rather than a `static_assert`.
- **The hook may run on another thread.** On a node that dispatches from several threads,
  the participant whose quiescent state completes the grace period runs the hook, which may
  not be the thread that called `unsubscribe()`. The hook must be short, thread-safe,
  non-blocking and `noexcept`.
- **Retirement is bounded.** Retired pairs wait in one shared table sized by
  `kDeferredReleaseSlots`; if every slot is taken the pair is dropped and its hook never
  runs — a deliberate leak in preference to a use-after-free. That `unsubscribe` answers
  `BACKPRESSURE`, and `graph_t::reclaim_refusals().release_refused` counts it.

## Source

```{literalinclude} /core/examples/sub_unsubscribe_from_dispatch.cpp
:language: cpp
:linenos:
```

See also: [reclamation policy](../reference/17-reclamation-policy.md) ·
[configuration](../modules/config.md) · [graph module](../modules/graph.md).
