# 15 — Concurrency and scaling (cross-cutting)

> **Scope.** §1–§5 are part of the standard: obligations any conforming implementation must
> meet, and properties of shared-memory hardware that constrain every one of them. §6 is a
> pointer. Numbers appear here only as *evidence for a general claim*, always with the host
> named, and never as a specification — see ["What this suite is NOT"](README.md).
> Measurements of the C++23 reference implementation live in
> [`docs/design/concurrency/`](https://github.com/avatarsd-llc/libtracer/blob/main/docs/design/concurrency/README.md), which is design material, not
> standard.

---

## 1. The short answer

Adding threads increases aggregate throughput **only when the threads touch different cache
lines.** One line that any thread *writes* inverts that, and how badly depends entirely on what
guards it:

- nothing shared, or shared read-only → throughput rises to the machine's ceiling;
- one atomic read-modify-write on a shared line → throughput **plateaus** at a constant;
- a *blocking* lock → plateaus at a worse constant;
- a *spinning* lock → throughput **falls** as threads are added.

Only the last case is genuinely retrograde, and it is the one people mean when they say
concurrency made things slower. §3 measures all four.

The practical consequence for libtracer is a design rule, not a tuning knob: **arrange for
readers to touch different vertices.** A vertex read by many threads at once is the one shape
where no implementation choice recovers linear scaling, because the value's reference count is
a single line every reader must modify (§4).

---

## 2. What the protocol requires

The wire protocol ([../spec/v1.md](../spec/v1.md)) says nothing about threads: it defines
frames and operations, and an implementation may be single-threaded. These are the obligations
that fall out of the operation semantics once an implementation *is* concurrent.

### 2.1 Obligations

| # | Obligation | Where it comes from |
| --- | --- | --- |
| O1 | A write to one vertex is **atomic with respect to reads of that vertex**: a reader observes the whole previous value or the whole new one, never a mix. | [02-graph-model.md](02-graph-model.md) — a vertex holds one value, and a value is a rope of views, not a byte range a reader may straddle. |
| O2 | A value handed to a reader stays valid for as long as the reader holds it, regardless of concurrent writes to the same vertex. | [08-views-and-ownership.md](08-views-and-ownership.md) — reads return *refcounted* handles. This is what forces the reference implementation's owning-read constraint (§4). |
| O3 | The **write sequence** of a vertex is monotone, and a subscriber's delivery order for one vertex matches it. | [04-communication-flows.md](04-communication-flows.md) — `await` waits for an increment; `IF_NEWER` delivery compares against it. |
| O4 | Registration may happen at any time, concurrently with reads, writes and awaits on other addresses. | [07-host-embedding.md](07-host-embedding.md) §handles — "registration is not init-only". |
| O5 | Refcount adjustment on a shared view uses atomic operations with at least acquire/release ordering; the *last* release must synchronize with every prior one. | [08-views-and-ownership.md](08-views-and-ownership.md) §refcount memory ordering. |

### 2.2 Deliberately left open

Anything an implementation may decide for itself, and which a second implementer should not
infer from the reference one: how many locks exist and what they cover; whether a read blocks;
whether a value's storage is reclaimed by a reference count, by an epoch, by hazard pointers,
or by never reclaiming until teardown; the order two concurrent writes to *different* vertices
appear in; and whether any operation is wait-free, lock-free, or blocking.

### 2.3 Where the protocol is silent and arguably should not be

**There is no stated ordering between a registration and a concurrent operation on the address
being registered.** O4 covers "other addresses". For the same address, an implementation is
free to answer a racing read either way, and nothing in the spec lets a caller tell which it
got. Implementations should not be surprised to find each other differing here; a caller that
needs the answer must order the two itself.

---

## 3. Four regimes

These are properties of cache-coherent shared memory, not of libtracer. An atomic
read-modify-write requires the line in an *exclusive* coherence state, so it must be taken away
from whichever core held it last; a plain read does not, and a line read by many cores can sit
in all their caches at once.

Measured with `bench/bench_contention`, which exists to let you check these on your own machine
rather than trust the table. **One host** — AMD Ryzen AI 9 HX PRO 375, 12 physical cores / 24
SMT threads, 24 MiB L3 in two instances, GCC 14 / libstdc++, medians of 5 runs. Aggregate
M ops/s; the ns column is system-wide time per completed operation (`1000 / aggregate`).

| regime | arm — what each thread does per op | T=1 | T=8 | T=24 | T24/T1 | ns @T=24 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| **(a)** disjoint | `local` — bump a private counter | 1360.3 | 4484.1 | 13005.4 | **9.56×** | 0.08 |
| **(a)** shared read-only | `shared-read` — read one never-written word | 1030.9 | 6103.1 | 10514.6 | **10.20×** | 0.10 |
| **(b)** one contended RMW | `rmw1` — one `fetch_add` on one atomic | 161.6 | 103.4 | 70.2 | 0.43× | 14.25 |
| **(b)** two | `rmw2` — two, same line | 72.7 | 49.3 | 35.8 | 0.49× | 27.90 |
| **(b)** four | `rmw4` — four, same line | 39.7 | 24.5 | 18.5 | 0.47× | 54.11 |
| **(c)** blocking lock | `rwlock` — `shared_mutex` read lock | 65.6 | 23.8 | 18.3 | 0.28× | 54.66 |
| **(c)** blocking lock | `mutex` — `std::mutex` | 72.0 | 19.0 | 22.9 | 0.32× | 43.74 |
| **(d)** spinning lock | `sp-load` — `atomic<shared_ptr>::load` | 41.8 | 2.6 | **1.4** | **0.03×** | 710.38 |
| — | `sp-copy` — copy a shared `shared_ptr` | 65.5 | 34.3 | 30.7 | 0.47× | 32.52 |

Spread over the 5 runs is 1.02–1.62× on the contended arms. It reaches 2.0–2.8× on `local` and
`shared-read`, which run near 1 ns/op where timer and scheduling noise dominate — treat their
absolute values as indicative and their *ratio* as the point.

Reading it:

- **The scaling ceiling here is ~10×, not 24×.** Twelve physical cores with two SMT threads
  each. Any claim that something "scales linearly" must be measured against ~10×.
- **(b) plateaus; it does not collapse.** Aggregate holds within 1.5× from T=2 to T=24, and
  per-op cost rises only 6.2 → 14.3 ns. A contended RMW costs *one* line transfer per
  operation however many threads want it, because the line can only be in one place at a time.
  ~14 ns × the number of RMWs is therefore a **floor** for any shared mutable counter on this
  host, and the arms are additive: 14.25 / 27.90 / 54.11 ns for one, two and four.
- **(c) plateaus lower, and a reader-writer lock is not the cheap option.** `mutex` beats
  `shared_mutex` at T=24 (43.7 vs 54.7 ns). Both cost only 3–4× their uncontended cost, because
  their losers *sleep* and leave the coherence traffic entirely.
- **(d) is the only retrograde case.** `sp-load` costs 30× more per op at T=24 than at T=1,
  because libstdc++ implements `std::atomic<std::shared_ptr<T>>` with a pointer-lock bit spun
  on with `lock cmpxchg`, no backoff and no queueing: every *loser*'s failed attempt is a full
  coherence transaction too, so traffic per *successful* operation grows with the thread count.

A curve that turns **down** is therefore evidence of coherency cost specifically. Amdahl's law
predicts a plateau from a serial fraction alone; the retrograde region requires the
crosstalk term that the Universal Scalability Law adds — `C(N) = N / (1 + α(N−1) + βN(N−1))`,
where it is `β` and not `α` that bends the curve back down.

**A flat aggregate is a serializer, not a success.** If T threads produce the same total as
one, each thread is T× slower, and the shape is indistinguishable from a perfect lock. This is
the single easiest mistake to make when reading a scaling table, and it hid a process-wide lock
in the reference implementation for months.

---

## 4. Why an owning read is a write

O2 says a read returns a handle the caller keeps valid. Some mechanism must therefore record
that the caller holds it — and whatever that mechanism is, it is *mutable state shared with
every other reader of the same value*. That puts every concurrent read of one vertex in regime
(b) at best: `sp-copy` above is exactly this cost, 32.5 ns at T=24 for the increment when the
handle is taken and the decrement when it is dropped.

No reclamation strategy removes it. Hazard pointers, epochs and reference counting differ in
what they do with *displaced* values, not in whether an acquired handle must be recorded. An
implementation escapes regime (b) only by weakening the contract — offering a **scoped** read
whose result may not outlive the call, so that nothing has to be recorded.

Two consequences a second implementer should plan for:

1. **A composed read over N addresses holds N handles at once.** Any scheme that can protect
   only one value per reader at a time cannot serve it directly.
2. **Read-only sharing is free, but O2 makes reads not read-only.** The bytes of a value can
   sit in every core's cache simultaneously; the bookkeeping cannot.

---

## 5. Which shapes scale

Independent of implementation, from §3 and §4:

| shape | scales? | why |
| --- | --- | --- |
| Threads reading **different** vertices | Yes | Disjoint lines — regime (a), up to whatever process-wide serializer the implementation has. |
| Threads writing **different** vertices | Yes | Same. |
| Threads reading **one** vertex | **No** | O2's bookkeeping is one shared line — regime (b) at best. |
| Threads writing **one** vertex | **No** | One value slot, one write sequence (O1, O3). Inherently serial. |
| One writer, many subscribers | Yes, in the writer | Fan-out is the writer's work; subscribers do not contend with each other. Cost grows with fan-out, not with core count. |
| Composed read of a subtree | Partly | N handles at once (§4); the fold is serial per reader. |
| Registration or retirement during traffic | No | O4 permits concurrency; it does not promise scaling. Expect a writer lock. |

The guidance that follows is about topology, not configuration: **a hot vertex is the shape to
design out.** Sharding one logical value across addresses, or letting consumers subscribe
rather than poll, moves the workload from the third row to the first, and no build-time or
runtime choice available in any implementation is worth as much.

---

## 6. The reference implementation

How the C++23 reference implementation places its locks, what each one costs, which of the
above it currently achieves, and what remains — all of it measured, all of it specific to one
codebase and one host — is [`docs/design/concurrency/`](https://github.com/avatarsd-llc/libtracer/blob/main/docs/design/concurrency/README.md). None of
it is normative, and a second implementer needs none of it.
