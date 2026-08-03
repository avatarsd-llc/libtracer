# Resolve-once reply-leg memo

**Rejected.** A per-delivery memo that caches the `deliver_remote` `by_name` link
resolution (`core/src/fwd_router.cpp` — the `registry_.by_name(sub.link)` scan opening
the remote delivery leg) so the reply path resolves a link once instead of per delivery.

## Why it keeps coming up

The scan is a visible per-delivery cost, so caching it looks free. It is not.

## Why it is out of scope

- **It was built, measured, and deleted.** The one-slot memo that recovered exactly this
  delta regressed the shipped four-link `reply-spread` fan-out by 140–311 ns/write — a
  latency regression on a shipped shape, which is a reject, not a trade. The experiment
  was removed once measured.
- **The prize is tiny and narrow.** The scan is 9–12 ns/delivery and only at a wide
  registry (W≈32); at typical widths (W≈4) the measured arms overlap and there is nothing
  to recover.
- **The wider design already ruled on it.** The value axis was wire bytes and per-hop
  descent on multi-hop routes, not the delivery-leg nanosecond — delivered by bound paths
  (PATH_REF), which deliberately do not touch the delivery leg. `reply-spread` is now a
  mandatory must-not-regress arm, so any single-slot reply memo is gated out by
  construction.

## If the underlying cost ever must move

Widening the registry's own `by_name` lookup is a different optimization — it pays off on
the forward and reply legs at once and is a registry data-structure change, not a
reply-leg memo. That would need its own narrowly-scoped ticket and its own measurement
against the same `reply-spread` bar; it is not this rejection.
