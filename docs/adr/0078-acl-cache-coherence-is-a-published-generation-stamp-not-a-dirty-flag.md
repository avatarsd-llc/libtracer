# ACL-cache coherence is a published-generation stamp, not a dirty flag

Status: **accepted; implemented** (2026-08-06, for [#880](https://github.com/avatarsd-llc/libtracer/issues/880)). `acl_cache_dirty` is gone and validity is derived from the invalidation counter, as decided — but the counter and the published stamp share ONE word rather than two, because two words cost a second load on the gate's fast path and the bench said so; see **Erratum 1** below, which is the shipped encoding. The acceptance instrument is `core/tests/acl_cache_race_test.cpp`: it reddens on the `{gen, dirty}` implementation (30/30 runs, ~4–5 k stale verdicts per run, both signs — fail-open and fail-closed) and greens on the shipped form (40/40 runs), with TSan clean on the same interleaving. That redden evidence covers the PREDECESSOR only; **Erratum 2** records what it took to cover the shipped CAS as well.

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

## Erratum 1 (2026-08-07, implementation) — the stamp is FOLDED INTO the counter, not a second word

The decision above stands unchanged; its **encoding** does not. Decision items 2–4 put `acl_published_gen` in its own word, which makes the consumer fast path two atomic loads instead of one. The consequence note demanded that be confirmed on the bench, and the confirmation **failed**: a pinned local A/B of `bench_libtracer acl` (25 interleaved samples per side, same core) measured the two-word form at **-1.2 % / -0.4 %** median throughput on `acl-inherit-d4` / `acl-inherit-d4-mt4`, with `P(candidate > base) = 0.34 / 0.29` and best-of-25 down 1.2 % / 1.1 % — about **+0.45 ns on a ~40 ns gate**, which is the extra load, not noise. A latency regression on the gate is not a trade this project takes, and the ADR itself made the choice conditional on there not being one.

So the shipped form keeps ONE word and spends its low bit:

- `acl_gen` is **odd ⇒ stale, even ⇒ `eff_aces` is the merge published for exactly this value**. It starts at `1` (never built ⇒ stale).
- Every invalidator advances it to the **next odd value** — `+1` from even, `+2` from odd — with one lock-free CAS (`vertex_t::invalidate_acl_cache`). It must advance even when already odd, or a rebuilder holding that odd snapshot would still win its publish.
- The rebuilder publishes by `compare_exchange_strong(snapshot, snapshot + 1)`. **The recheck and the publish are now the same atomic operation**, which is strictly stronger than decision item 3: there is no interval at all between "no write raced me" and "this merge is current".
- The consumer fast path is `(acl_gen.load(acquire) & 1) == 0` — one load and a parity test, exactly what the retired dirty flag cost.

Re-measured with the same instrument, the shipped form is **faster than the pre-fix baseline**: +1.4 % / +0.8 % median throughput, `P(candidate > base) = 0.66 / 0.71`, p99 70 → 61 ns on `acl-inherit-d4`, and 248 B less `.text`. `vertex_ext_t` also loses 4 bytes (the flag plus its padding).

Two costs are accepted with it. The invalidator's single `fetch_add` becomes a CAS loop — contended only against another invalidator of the same vertex or that vertex's rebuilder, on the control-plane `:acl` write path, never on the gate. And the usable generation space halves to 2^31, since the low bit is no longer a counter bit; the width argument above is unaffected at that order of magnitude.

Rejected alongside it: **packing a 32-bit stamp and a 32-bit counter into one 64-bit atomic**, which would also be a one-load fast path but is not lock-free on the 32-bit MCU targets this core must serve.

## Erratum 2 (2026-08-08, instrument) — the acceptance evidence covered the PREDECESSOR, not the shipped CAS

The decision and the shipped encoding both stand. What was overstated is the coverage the acceptance instrument gave them, and [#1043](https://github.com/avatarsd-llc/libtracer/issues/1043) is where it was caught: a lost-update mutant of the shipped publish —

```cpp
std::uint32_t expected = gen;                                          // shipped
if (!e.acl_gen.compare_exchange_strong(expected, gen + 1, ...)) continue;

e.acl_gen.store(gen + 1, std::memory_order_release);                   // mutant
```

— **survived** `acl_cache_race_test` as it stood: 0 stale verdicts in 39 293 bracketed claims (RelWithDebInfo) and again under the TSan/`hazard_slot_t` CI leg. It is the same defect class the CAS exists to prevent (the concurrent mark is discarded and a merge assembled over pre-write ancestor ACEs is stamped current), so on the mutation-testing reading the load-bearing line of this ADR was unguarded at the observable level. The 30/30 figure above was measured against the `{gen, dirty}` protocol, which is a different implementation with a different window.

**Why it survived is the walk order, not the epoch count.** `graph_t::acl_allows`'s rebuild lambda walks the parent chain from the bearer UPWARD, so the level the writer rewrites decides which side of that read gets the instrument's deliberately expensive filler load:

| rewritten level | read in the walk | the wide interval | which lost update it reaches |
| --- | --- | --- | --- |
| `/anc` (**TOP**, as originally shaped) | LAST | after the publish decision — `eff_aces = std::move(merged)` | the predecessor's `dirty = false` clear |
| the bearer's own parent (**NEAREST**) | FIRST | between the ancestor read and the publish | the shipped publish CAS |

On TOP, the interval a lost mark must land in holds one single-ACE `append_ancestor`, the merge release and an uncontended stripe re-acquire — which is why ~40 k claims never sampled it. Running more epochs was never going to fix that.

Both shapes now run. On NEAREST the same mutant reddens **10/10 RelWithDebInfo runs at 1 089–2 575 stale verdicts each**, and the TSan Debug/`hazard_slot_t` run at **3 420** — both pinned to two cores — and the sign is not fixed: the first stale verdict was fail-OPEN (`expected DENY, got ALLOW`) on the RelWithDebInfo runs and fail-CLOSED on the TSan one. TOP reported 0 stale in every one of those same runs, so neither shape subsumes the other and both are kept.

A racer's exposure is still the host's to grant, so the file also gained a DETERMINISTIC guard on the same line, `test_mark_inside_rebuild_defeats_the_publish`, driving `vertex_t::with_effective_aces` directly. The rebuild is a caller-supplied callable that runs with the stripe lock released, and `mark_acl_cache_dirty` is lock-free and takes no vertex lock, so firing the mark from inside the rebuild reproduces the protocol interleaving exactly — thread identity is not part of the protocol, only the order is. It fails the mutant on every run and on a loaded host, which is the property the racer cannot promise.

The correction this erratum makes to the record is narrow and worth stating plainly: **"the acceptance instrument reddens on the defect" was true of the protocol this ADR replaced, and was read as though it were true of the protocol it shipped.**
