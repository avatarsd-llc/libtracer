# Reclamation

## In one paragraph

A delivery copies a vertex's edges and calls each subscriber outside every lock, so another
thread may unsubscribe while that copy still names the subscription. Its context must not be
freed until every such delivery has ended. The rule that decides when is `reclaim_policy_t`, a
compile-time choice. The default on every target, `reclaim_qsbr_t`, is quiescent-state
reclamation: each thread marks itself online while it dispatches and offline when it leaves, and
a retired subscription is released once every thread that was online has left. The opt-out,
`reclaim_strict_t`, tracks nothing and releases at once; it is sound only on one core with no
preemption. Either way the application learns of the release through one hook, run exactly once.

## The picture

```{mermaid}
:caption: "Legend: purple = application, sky blue = record, green = vertex."
flowchart LR
    accTitle: An unsubscribe while another thread is delivering
    accDescr: Thread B unsubscribes while thread A is mid-delivery. If no thread is mid-dispatch the release hook runs at once on thread B. Otherwise the retired pair waits in a bounded shared table, and the thread whose dispatch exit completes the grace period runs the hook. If the table is full the pair is dropped, its hook never runs, and unsubscribe answers BACKPRESSURE.
    B([thread B: unsubscribe]):::app --> Q{"any thread mid-dispatch?"}
    Q -->|"no"| H["hook runs now, on B"]:::record
    Q -->|"yes"| T["deferred table, kDeferredReleaseSlots"]:::record
    A([thread A: delivering]):::app -->|"dispatch exit ends the grace period"| T
    T -->|"hook runs on A"| R("subscriber context freed by the app"):::vertex
    Q -->|"table full"| X["dropped, BACKPRESSURE, release_refused + 1"]
```

## When you need it

When a subscription's context is heap memory, a C++ object or anything else the application
frees, and more than one thread touches the graph. It also decides how a NARROW build may skip
the bookkeeping altogether.

## How it works

**The bracket.** The outermost dispatch on a thread does one relaxed load of a global epoch, one
release store into the thread's own cell and then a fence. Leaving stores zero, so a thread
asleep in `epoll` holds nothing and never stalls a grace period. The fence is chosen at build
time: `seq_cst` by default, a compiler barrier under `kSingleCore`, or a plain store with the
reclaim side's `membarrier(2)` under `kQsbrAsymmetricFence` on Linux.

**Unsubscribe.** `graph.unsubscribe(sub, &on_dead)` removes the edge at once, then bumps the
epoch and scans the threads. With none mid-dispatch, the hook runs before `unsubscribe` returns.
Otherwise the pair waits in one shared table, and the thread whose exit completes the grace
period runs the hook. The hook may therefore run on another thread: keep it short,
thread-safe, non-blocking and `noexcept`, and post the context to a queue the application owns
if it must be freed elsewhere.

**Overflow.** A full table drops the pair: its hook never runs, a deliberate leak rather than a
free under a live delivery. `unsubscribe` answers `BACKPRESSURE`, and
`graph_t::reclaim_refusals()` counts it as `release_refused`.

**Strict.** `reclaim_strict_t` runs the hook inline on the caller. Unsubscribing from inside a
delivery is forbidden under it and not detected. `reclaim_local_t` is refused at compile time.

**Other parked memory.** A thread that never dispatches but displaces last-known values calls
`graph_t::thread_quiescent()`. Value seams a retire parks are freed by `graph_t::collect()` on the
caller's thread, outside every graph lock.

**Shard rather than wait.** A subscription dispatched and unsubscribed by one thread is always
released inline. Sharding subscriptions per receive thread keeps the grace period from waiting.

## Cost

### Wire bytes

None.

### Allocations

None. The deferred table is `kDeferredReleaseSlots` entries, 16 by default, fixed at build time;
a `reclaim_strict_t` build links none of it.

### Latency

WIDE `forward`, `publish1` and `publish8` at p50: 130, 100 and 110 ns under QSBR, against 120,
90 and 100 ns under strict. The bracket costs per frame or per edge, never per byte: the 1, 4
and 16 KiB rows show the same as 64 B. On one core it is about 22 instructions and no fence.

### Flash and RAM per target

| Target | Policy | Per-thread state |
| --- | --- | --- |
| NARROW | `reclaim_strict_t` on one core without preemption, else QSBR | none under strict |
| MID | `reclaim_qsbr_t` | 64 B per FreeRTOS task |
| WIDE | `reclaim_qsbr_t` | 16 B per thread |

## What the library does not do

- It does not block `unsubscribe` to wait for a grace period.
- It does not use a timer or a clock to end one.
- It does not promise the hook runs on the thread that unsubscribed.
- It does not detect an unsubscribe from a delivery under `reclaim_strict_t`.

## Pitfalls

- **Symptom:** a use-after-free in a delivery callback. **Cause:** the application frees the
  context when `unsubscribe` returns, not in the hook. **Fix:** free it only from `on_dead`.
- **Symptom:** `release_refused` climbs. **Cause:** more retirements wait than the table holds.
  **Fix:** raise `kDeferredReleaseSlots`, or shard so each unsubscribe concludes inline.
- **Symptom:** a crash after an unsubscribe on a dual-core MCU. **Cause:** `reclaim_strict_t` on
  a build with a second core or preemption. **Fix:** keep `reclaim_qsbr_t`.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope). When a node frees memory is an
implementation concern and not normative.

**Exercised by:** `reclaim_test`, `reclaim_refusal_test`, `reclaim_local_refused`,
`qsbr_drain_claim_test`, `qsbr_epoch_wrap_test`, `qsbr_exit_order_test`, `qsbr_mc`,
`lkv_slot_test`, `subscribe_retire_race_test`, `terminus_retire_race_test`.

## History

:::{dropdown} How reclamation got here
- [ADR-0069](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md)
  made the last-known-value slot a compile-time policy, with a hazard-slot binding for WIDE.
- [ADR-0080](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md)
  made reclamation a build-time policy per target.
- 2026-10-12: ADR-0080 Amendment 1 made QSBR the one grace mechanism and the default, and
  withdrew `reclaim_local_t`.
:::

Next concept: [Ordering](ordering.md). Go deeper:
[reference 17](../../reference/17-reclamation-policy.md#what-each-policy-actually-does), then
[what it costs](../../reference/17-reclamation-policy.md#what-it-costs--measured).
