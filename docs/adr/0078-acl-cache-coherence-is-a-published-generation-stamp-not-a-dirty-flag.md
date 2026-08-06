# ACL-cache coherence is a published-generation stamp, not a dirty flag

Status: **proposed** (2026-08-06, for [#880](https://github.com/avatarsd-llc/libtracer/issues/880)).

The per-vertex effective-ACE cache is invalidated **lock-free** from an ancestor `:acl` write (`mark_acl_cache_dirty`, `core/include/libtracer/vertex.hpp:1854-1864`, fanned out by `graph_t::mark_subtree_acl_dirty` under only `shared_lock(map_mutex_)`), while it is rebuilt under the vertex stripe lock. We decide that cache validity is expressed as a single published-generation stamp compared against the invalidation counter — `acl_published_gen == acl_gen` — and **not** as a separate `acl_cache_dirty` boolean that the rebuilder clears. The boolean is removed.

## Context

The shipped protocol is a `{acl_gen, acl_cache_dirty}` pair with a lost-update window:

- **Invalidator (lock-free):** `e->acl_gen.fetch_add(1, release); e->acl_cache_dirty.store(true, release)` — no stripe lock.
- **Rebuilder (stripe lock held), `vertex.hpp:1928-1938`:** snapshots `gen = acl_gen`, drops the lock to rebuild, retakes it, then does **two separate** atomic ops — a generation recheck (`if (acl_gen.load() != gen) continue;`, `:1934`) followed by `acl_cache_dirty.store(false)` (`:1937`).
- **Consumer:** `acl_allows` (`graph.cpp:751-806`) fast-paths on the flag alone, holding no map lock and storing no generation stamp beside the cache.

The interleaving: the rebuilder passes its gen recheck → an ancestor `:acl` write bumps `acl_gen` and stores `dirty=true` → the rebuilder's `dirty=false` store lands **on top of** that mark and publishes `eff_aces` built from the pre-write ancestor chain. The flag is now clean, the cache is stale, and nothing later detects the mismatch. Every subsequent `acl_allows` on that vertex evaluates the stale merge until the next `:acl` mutation anywhere in the chain — a persistent, silent authorization error whose sign (fail-open or fail-closed) is whatever the pre-write merge happened to encode. Because the consumer is on the security path, a persistent stale-open is the headline risk.

The root cause is structural: **two independent pieces of state (a counter and a boolean) that must agree, mutated by two parties, one lock-free.** The rebuilder's clear of the boolean is a check-then-act over state a lock-free writer also owns.

## Decision

Collapse validity onto one monotonic counter:

1. Keep `acl_gen` as the invalidation counter; every invalidator does `acl_gen.fetch_add(1, release)` and **nothing else** — there is no second flag to store, so no store can lose the race.
2. Add an atomic `acl_published_gen` beside `eff_aces`, initialized to a value that cannot equal the first real `acl_gen` (so a never-built cache reads as stale).
3. The rebuilder, under the stripe lock, snapshots `gen = acl_gen` **before** its walk, rebuilds, and on success publishes `acl_published_gen.store(gen, release)`. If `acl_gen` moved during the walk, the published stamp is already behind the counter, so the cache reads stale and is rebuilt again — the recheck-and-`continue` becomes implicit in the comparison rather than a separate clear.
4. The consumer fast path is `acl_published_gen.load(acquire) == acl_gen.load(acquire)`; unequal ⇒ rebuild.

A `fetch_add` by any invalidator now invalidates unconditionally and cannot be clobbered, because the only writer of `acl_published_gen` is the stripe-lock-holding rebuilder and it only ever advances it to a value it snapshotted before the invalidation it might have missed.

## Considered options

- **Dirty boolean cleared by the rebuilder (status quo).** Rejected: the defect. A boolean the rebuilder clears is a lost-update against a lock-free setter.
- **Take the stripe lock in the invalidator.** Rejected: the invalidator is fanned out across a whole subtree from an `:acl` write holding only `shared_lock(map_mutex_)`; acquiring every descendant's stripe lock there would serialize subtree invalidation against all evaluation and rebuild traffic, and the whole point of the lock-free mark is that a control-plane-rare `:acl` write must not stall the evaluation hot path. The published-generation stamp keeps the invalidator lock-free *and* correct.
- **Store the generation inside `eff_aces` itself / version the pointer.** Equivalent in effect; a separate atomic stamp beside the cache is the smaller change and avoids widening the published cache object.

## Consequences

- The invalidation path loses one atomic store; the consumer path swaps a boolean load for one extra atomic load and a compare — negligible, and on the security-evaluation path a persistent stale-open is the thing being removed, so any micro-cost is not a trade worth declining. Confirm with the existing ACL/eval bench that the compare does not regress.
- The generation counter must be wide enough that wraparound to a stale-but-equal value is not reachable in practice; a 32-bit counter at control-plane `:acl`-write frequency is safe for any realistic uptime, but the width choice is now load-bearing and is recorded here.
- A **hammer test** is the acceptance instrument: one thread rewriting an ancestor `:acl`, one thread evaluating `acl_allows` on a descendant, asserting the post-write ACE set is observed within a bounded number of evaluations. This test must redden on the `{gen, dirty}` implementation and green on the stamp.
