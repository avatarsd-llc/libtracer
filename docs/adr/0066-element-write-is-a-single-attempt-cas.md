# 66. An element write is a single-attempt CAS that answers `BACKPRESSURE`, never a retry loop and never a silent overwrite

Status: accepted (maintainer-ratified 2026-07-28 across a grill-with-docs walk of [RFC-0017](../spec/rfcs/0017-element-addressing-value-plane-index.md)). Implements the concurrency half of RFC-0017 §C. Upholds [ADR-0064](0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md) (the publish path stays waiterless), [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) §2 (exhaustion is backpressure), and [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md) (bounds are injected resources, never magic constants).

## Context

[RFC-0017](../spec/rfcs/0017-element-addressing-value-plane-index.md) makes `/path[n]` address the n-th child TLV of a vertex's stored value. Every operation it adds — replace child `n`, clear child `n`, append one child — is a **read-modify-write**: load the published value, splice, publish the result.

The existing write path is not one. `vertex_t::store` publishes with a single atomic store:

```cpp
std::shared_ptr<const rope_t> sp = try_make_lkv(std::move(value), mr);
if (!sp) return nullptr;          // OOM soft-fail (#477) -> BACKPRESSURE
lkv_.store(sp);                   // publish; no CAS, no lock
```

and the waiterless path (#555, #370) then takes **no mutex at all**. That is deliberate and measured: `lkv_` is `std::atomic<std::shared_ptr<const rope_t>>`, which `is_lock_free()` reports as 0 on libstdc++, so the store already costs an internal pointer-lock acquire — roughly **77 of the ~316 cycles of an in-process write, the largest single term left on the path** (recorded in `vertex.hpp` and in [ADR-0064](0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md) §2, tracked as [#604](https://github.com/avatarsd-llc/libtracer/issues/604)).

Whole-value last-writer-wins is coherent *because* every write replaces everything: an operation whose intent is "set to X" losing to one whose intent is "set to Y" is a serial outcome. An element write breaks that assumption, because it **derives its output from a read**. Read-derived writes are exactly where last-writer-wins stops being equivalent to serial execution.

The concrete failure, with a vertex holding a queue `[a, b]`:

| Step | Consumer (delete element 0) | Producer (append `c`) |
| ---- | ---- | ---- |
| 1 | loads `[a, b]` | |
| 2 | | publishes `[a, b, c]`, gets `OK` |
| 3 | publishes `[b]` | |

`c` is gone. It was never delivered to any subscriber and never will be, and both writers were told their operation succeeded. This is not a stale read that the next write repairs — it is the node's stored state moving **backwards**, silently and unattributably.

## Decision

**An element write publishes with a single-attempt compare-exchange against the value it read. On conflict it applies nothing and answers `BACKPRESSURE`. It never retries internally, and it never publishes unconditionally.**

```
load  cur = lkv_
build next = splice(cur, n, payload)          // one allocation, from the injected resource
if   (!lkv_.compare_exchange_strong(cur, next))   -> discard next, answer BACKPRESSURE
```

Four properties, each of which was the reason a neighbouring option lost:

1. **No lost newer data.** The write either applies to the state it read or does not apply. Storage never moves backwards.
2. **The plain-write fast path is untouched.** `store()` keeps its single unconditional publish — no new lock, no CAS, no branch. A node that never uses `[n]` pays exactly nothing, which matters because that path is already the measured bottleneck (#604).
3. **No starvation inside the node.** A hot producer cannot make a consumer's delete spin, because there is no loop to spin in. The retry policy lives with the caller, which is where this project puts policy.
4. **No invented constant.** A bounded retry count would be a magic number, which [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md) forbids — bounds come from injected resources or per-target config. "One attempt" is not a tuning parameter; it is the absence of one.

`BACKPRESSURE` is reused rather than added: `store()` already maps its OOM soft-fail to it, so a caller that handles a failed write handles this. RAM is bounded by **one discarded rope per in-flight element writer** — allocator churn under contention, never growth.

**Scope.** This governs the element operations of RFC-0017 only. Whole-value writes keep last-writer-wins unchanged; nothing about their contract, cost, or code path moves.

## Consequences

**Positive**

- Element writes are linearizable with respect to whole-value writes without serializing them.
- The hot write path keeps the waiterless, lockless shape #555 and #370 bought.
- Contention surfaces as an explicit, retryable status instead of silent data loss.
- When #604 eventually makes `lkv_` genuinely lock-free, the CAS gets cheaper with it — this decision adds a second consumer of an already-scheduled fix rather than a new problem.

**Negative / risks**

- Callers must handle a retryable status on element writes. This is a real ergonomic cost, accepted because the alternative is an unsignalled loss.
- Under sustained contention an element write can fail repeatedly and make no progress. That is a liveness property the caller can observe and act on — unlike a retry loop, where the same contention is invisible and unbounded.
- `compare_exchange` on `std::atomic<std::shared_ptr<T>>` takes the same internal pointer-lock as `store` on libstdc++, so an element write pays that cost on **every** attempt including failures. Acceptable while element writes are a control-plane-rate operation; it is a reason not to promote them onto a per-sample data path.

## Considered options

**Last-writer-wins, no retry** (load, splice, store unconditionally). Cheapest — zero hot-path cost, single pass, never starves, and superficially consistent with the existing whole-value model. Rejected on the scenario above: it does not merely drop the *element* write, it discards a concurrent producer's **newer whole value**, with an `OK` ack on both sides and nothing on the wire recording it. Consistency with last-writer-wins is only apparent; whole-value writes do not read before they write.

**A retrying CAS loop.** The strongest guarantee — the element write always lands and the caller never sees a conflict. Rejected twice over: a hot producer can starve it indefinitely, and any bound on the retries would be a constant this project does not permit itself to invent. It also hides contention that the caller may need to know about.

**Serialize element and plain writes under the vertex stripe mutex.** Correct and simple to reason about. Rejected on the priority ordering: it puts a mutex back on the plain write path that #555 and #370 deliberately removed, so every writer on every vertex pays to protect an operation most vertices never perform.

**A per-vertex version counter checked before publish.** Equivalent to the CAS in effect but strictly worse in mechanism: it needs a second atomic and a second serializing operation next to one the code already has, and `lkv_`'s own pointer identity is already the version.

## Relates

- [RFC-0017](../spec/rfcs/0017-element-addressing-value-plane-index.md) — the normative surface this implements.
- [ADR-0064](0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md) §2 / [#604](https://github.com/avatarsd-llc/libtracer/issues/604) — the reclamation ruling that makes `lkv_` genuinely lock-free; this decision benefits from it and does not block on it.
- [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md) — the failable-allocation seam the splice allocation draws from.
