# The LKV publish takes no lock when nobody is awaiting, and the slot itself becomes lock-free

Status: **§1 accepted and implemented; §2 proposed.** Extends [ADR-0060](0060-lkv-copy-store-injected-value-backend.md) (the LKV copy-store and its injected `value_backend_`) on the *synchronisation* axis rather than the allocation one. Preserves [ADR-0021](0021-pay-for-what-you-use-machinery.md)'s pay-for-what-you-use rule and the `#361 §2` stripe design that replaced per-vertex blocking primitives. Does not touch [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) §3 — the FWD demux was already lock-free and stays so. Grounded in cycle-level measurement of the in-process write path; see [#555](https://github.com/avatarsd-llc/libtracer/issues/555).

## Context

The in-process write is the floor under every network number this project quotes, and it is the path a local subscriber actually takes — the ordinary ROS-node shape, where a sensor publishes and consumers in the same process read. It had never been profiled.

Measured on `main`, Release `-O3`, pinned, in **cycles per operation** — not the bench's own `p50`, which [cannot resolve differences under ~10 ns](https://github.com/avatarsd-llc/libtracer/issues/553):

| phase | cycles/op |
| --- | ---: |
| graph write, 0 subscribers (the store leg) | 233 |
| `fan_out` to one callback subscriber | 71 |
| **total** | **~335 (~72 ns)** |

Inside that, `perf annotate` localises **~77 cycles (~17 ns) to the `lkv_.store(sp)` publish alone**, and a further **~38 cycles to the stripe mutex taken immediately after it**.

### The finding that reframes the path

Three experiments, each measured rather than argued:

- Constructing `edge_view_t` in place in `snapshot_edges` instead of local-then-move — **−164 instructions/op, −0 cycles.** Baseline runs at IPC 5.1; the out-of-order machine absorbed the removal entirely.
- Removing the atomic `shared_ptr` publish alone (semantics-breaking, for sizing only) — **also 0 cycles.**
- Removing the atomics **and** both stripe locks — **−66 cycles.**

**The path is not work-bound.** Roughly 1700 instructions of honest work are hiding at near-peak IPC underneath a short chain of `lock`-prefixed read-modify-writes, each of which drains the store buffer. Remove one link and the next still stalls you; only reducing the *count of serializing operations* moves the number. That is why every "do less work" lever on this path measures zero, and it is the single most useful thing to know before touching it again.

A second measurement worth recording, because the codebase asserted the opposite: a leaf write makes **exactly one** allocation (the 104-byte `allocate_shared` of the LKV rope), and removing it outright by injecting a pool buys **under a nanosecond**. The write path is not allocation-bound either. ADR-0060's `value_backend_` does not serve it at all — that seam covers `materialize()`/flatten on *branch* and *field* writes.

### Where the two locks differ

A write took the stripe mutex twice. Sizing them by deletion:

| variant | cycles/op | Δ |
| --- | ---: | ---: |
| baseline | 341 | — |
| `store()` lock removed only | 304 | **−38** |
| `snapshot_edges()` lock removed only | 346 | **0** |
| both removed | 314 | −27 |

They cost radically different amounts despite removing the same ~160 instructions each. `store()`'s sits immediately downstream of the atomic publish, so the two serializing regions land back-to-back on the critical dependency chain. `snapshot_edges()`' overlaps the tail and is **free as it stands** — a fact worth pinning, so nobody spends effort removing it.

## Decision

### §1 — A publish with no awaiter takes no lock (implemented)

`write_seq_` and the stripe's `waiters` count become atomic. A non-`STREAM` publish bumps the sequence with one `fetch_add(seq_cst)`, reads `waiters`, and **returns without touching the mutex** when it is zero. `STREAM` roles keep the original shape verbatim: the ring append is real state mutation and must stay under the lock.

This finishes what `#370` started. That change already skipped the *condvar call* on the waiterless path; the mutex itself was what remained, and it was not free.

**Why no wakeup can be lost.** Both sides are `seq_cst`, so they share one total order:

- **Writer:** bump `write_seq_`, *then* read `waiters`.
- **Waiter:** publish `++waiters`, *then* read `write_seq_` to evaluate its predicate.

If the writer reads `waiters == 0`, it is ordered before the waiter's store, hence before the waiter's read of the sequence — so the waiter observes the bump and `wait_for` returns on its **first** predicate evaluation, without ever blocking. If the waiter got there first, the writer reads a non-zero count and takes the slow path, which acquires the very mutex the waiter must hold in order to block. Neither interleaving leaves a sleeper.

A spurious slow path is harmless and already contemplated by the stripe design: `waiters` is per *stripe*, so an unrelated vertex's awaiter makes this publish take the lock and notify needlessly — the same collision `vertex_stripe_t` documents as "a spurious wake plus a re-check, never a correctness change".

`current_seq()` becomes lock-free as a consequence: the sequence is atomic and a publish no longer holds the mutex while bumping it, so taking the lock there would synchronise against nothing.

**Measured:** `inproc` **89.2 → 82.6 ns/op** (min of 5 pinned interleaved rounds, faster in 5/5), ~−20 cycles/op. 61/61 tests pass, 61/61 under TSan, the `allocs=0` forward gate is byte-identical, and `bench_await_wakeup_storm` delivers wakeups at every waiter count from 1 to 128.

The single-threaded gain is ~7%. **The real prize is multi-writer contention**, which no bench in this repo currently measures — a stripe is shared by 16 vertices by default, so today every publish on any of them serialises against every other.

### §2 — The LKV slot itself becomes lock-free (proposed, not implemented)

`std::atomic<std::shared_ptr<T>>::is_lock_free()` returns **0** on this libstdc++. The "lock-free atomic `shared_ptr`" the codebase describes is lock-free *by contract* and **spin-locked in implementation** — both load and store acquire libstdc++'s pointer-lock bit. The disassembly is explicit: `lock cmpxchgq` to acquire the bit, the pointer and refcount stores, then `xchgq` to release it, and 88% of `vertex_t::store`'s samples land on those instructions.

That is ~77 of the ~316 remaining cycles, and it is now the largest single term on the write path. It is also paid by every **read**, since `read_stored()` takes the same bit.

The proposal is to replace the slot with a genuinely lock-free one: a single atomic pointer to an immutable `rope_t`, published with `release` and read with `acquire`, plus a reclamation scheme for the displaced value.

**Reclamation is the whole problem, and it is why this is a separate decision.** A reader that has loaded the pointer but not yet incremented a refcount must not have the object freed under it. The candidate schemes:

- **Epoch-based reclamation** — readers announce an epoch; a writer frees only what no reader can still reference. Cheap on the read side (a store, no RMW), but needs a per-thread registry, which is real state on a target where per-vertex bytes are already fought over.
- **Hazard pointers** — a reader publishes the pointer it is about to use; the writer scans hazards before freeing. Bounded memory, but the read side pays a `seq_cst` store plus a fence, which is much of what we are trying to remove.
- **Deferred reclamation on the existing park list** — `graph_t::retired_seams_` already implements exactly this discipline for the handler seam (ADR-0057's insert-only rule extended: emptied, never dangled). Reusing it costs no new mechanism, but it never reclaims before teardown, which is acceptable for a seam swapped once per retirement and **not** acceptable for a value replaced on every write.

None of these is obviously right, and the honest expected win is **~15 ns on a ~67 ns path** — real, but bought with a reclamation scheme across the whole L4 surface.

**This ADR does not choose one.** It records that the term exists, that it is now the largest one, that the "lock-free" language in the code is currently inaccurate, and that any attempt must come with its own measurement and its own reclamation argument. A follow-on ADR should make that choice.

## Considered options

- **Do nothing (§1).** Rejected on measurement: −38 cycles is 11% of the write, it is reproducible, and the change is confined to one verb with a stated ordering argument.

- **Remove the `snapshot_edges` lock too.** Rejected — measured at **0 cycles**. It is free where it sits, and removing a lock for no gain spends correctness budget on nothing.

- **Take the fast path for `STREAM` roles as well.** Rejected: the ring append is genuine state mutation. `STREAM` keeps the original code verbatim so the change cannot regress a role it was not measured on.

- **A lock-free slot now, in the same change (§2).** Rejected on scope. It needs a reclamation scheme, it touches every reader, and folding it into a change whose correctness argument is a two-line Dekker pair would make both unreviewable.

- **Replace the stripe mutex with a spinlock.** Rejected for the reason [ADR-0063](0063-connection-table-lock-free-reads-trait-serialized-writes.md) erratum 1 rejected it on the control plane: on a single-core FreeRTOS target a high-priority task spinning on a lock held by a lower-priority one is unbounded priority inversion with no way out. A mutex has priority inheritance.

## Consequences

- **`write_seq_` and `waiters` are now part of a documented ordering contract.** Anything that touches either must preserve the `seq_cst` pairing in §1 — a relaxed store on the waiter side, or reading `waiters` before bumping the sequence on the writer side, silently reintroduces the lost-wakeup window. The argument lives next to the code, not only here.

- **`current_seq()` is lock-free**, so callers no longer serialise on the stripe to read a sequence.

- **The multi-writer win is unmeasured.** No bench in this repo exercises concurrent publishers on one stripe, so the contention benefit is argued, not shown. A bench that does would also make §2's case measurable, and is the natural prerequisite to attempting it.

- **The "lock-free LKV" language in the code is currently wrong** wherever it describes the `atomic<shared_ptr>` load as lock-free. §2 is what would make it true; until then the comments should say *lock-free by contract, spin-locked in libstdc++*.

- **`STREAM` publishes are now measurably more expensive than others**, because they alone still take the mutex. That asymmetry did not exist before and should be stated wherever STREAM's cost is discussed.
