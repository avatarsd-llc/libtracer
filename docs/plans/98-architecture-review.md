# Architecture Review — libtracer v0.1 Design

> **Status**: critical review, 2026-05-03. Not a marketing document. The goal is to surface the things this architecture cannot do, the things it pretends it can, and the conditions under which the design's load-bearing claims collapse. Read this before declaring v0.1 frozen.
> **Audience**: the maintainer and any second-implementer evaluating whether to commit. Anyone benchmarking libtracer against Zenoh / Iceoryx2 / DDS who needs to know where the comparison is fair and where it isn't.
> **Method**: walked the [reference suite](../reference/) section by section with two adversarial scenarios in mind — (a) gigabytes of streaming data with low end-to-end latency budgets, (b) microsecond-class control loops on tiny MCUs and on multi-socket Linux servers. Whatever the design says, the questions are: *under what conditions does it stop saying that*, and *what does it cost when it does*.

---

## Executive verdict

The design is internally consistent, intellectually honest about what it skips (no consensus, no CRDTs, no security in v0.1), and the same-substrate insight (TLV-in-memory IS graph-node IS wire-bytes) is genuinely novel and credible at small-to-medium scale. The wire format is small, branchless, and aligned. The vertex-facade principle gives one address space across radically different storage and transport realities.

But the architecture has **five blockers** that prevent it from being a serious replacement for Zenoh / Iceoryx2 / DDS in production deployments above a few thousand vertices or a few hundred MB/s:

1. ~~**Path resolution cost is unspecified**~~ **RESOLVED 2026-05-06** — [../spec/v1.md](../spec/v1.md) §3.1 now mandates path handles. Hot-path API takes a handle (a pointer to a pre-encoded PATH TLV in `.rodata` or an init-time-allocated heap segment); dispatcher's vertex map is keyed on canonical PATH TLV bytes ([../reference/02-graph-model.md](../reference/02-graph-model.md) §dispatch keyed on canonical PATH TLV bytes). No `snprintf`, no parser walk, no allocation per write. P0 builds MAY omit the string-form entry entirely. Original concern: every read/write/subscribe walked a UTF-8 string against a tree, which would have dominated the latency budget at the documented rates.
2. **Address-shift slicing imposes O(N) dispatch overhead per logical message** at the L4 layer — even though L0/L1 are zero-copy, sending a 1 GB frame as 16384 × 64 KiB slices means 16384 path resolutions, 16384 wildcard match passes, and 16384 entries churning through the bridge dedup recent-set. The bandwidth claim survives; the messages-per-second claim does not.
3. **Refcount memory ordering is correct but unsharded** — fan-out under contention reduces to a single hot cache line per segment. At 4+ subscribers cloning a hot vertex's view from independent CPU cores, the contended atomic dominates the latency budget at the µs scale.
4. **Bridge dedup recent-set sizing is fundamentally a function of network rate × delivery window**, not of "expected_max_fanout × publish_rate" as currently documented — and degrades badly under bursts. Eviction on bursts is when cycle storms happen, not when the network is calm.
5. **No flow control in the protocol**. QoS deadlines and best-effort drops are described, but the protocol gives no native back-pressure signal. A slow subscriber consumes memory until the publisher's segment pool is exhausted; the failure mode is a slow latency degradation followed by a cliff.

The **eight blind spots** below explain what the spec doesn't address at all (security, schema versioning, per-transport multiplexing, etc.).

The design is **shippable** for tightly-scoped deployments — RC car, single-bus robot, small fleets. It is **not** shippable as a general-purpose middleware until the blockers above are addressed. The roadmap [02-roadmap-weeks-1-to-8.md](02-roadmap-weeks-1-to-8.md) is a sound 8-week plan to validate the small-deployment story; it does not address any of the five blockers.

---

## Stress test 1 — Gigabytes of streaming data

The canonical motivating scenario is in [00-overview.md](../reference/00-overview.md) §the five load-bearing claims #5: "a 10-GB camera frame fits across `ep[0..N]` slices." Let's trace what happens at production rates.

### Workload A — 4K video at 30 fps over multicast UDP

- Frame size: 3840 × 2160 × 4 bytes = 33.18 MiB.
- Frame rate: 30 fps.
- Sustained bandwidth: ~1 GB/s. (RAW; with Bayer/JPEG it's lower, but let's assume RAW for the stress.)
- Slice size: 64 KiB (the design's typical default).
- Slices per frame: ~531.
- Slices per second: ~15,930.

**What the protocol does well here:**

- Every slice is a 64 KiB TLV with a 4-byte header and (typically) a 12-byte trailer. Per-slice overhead: ~16 bytes / 65536 = 0.024 %. Header overhead is negligible.
- Each slice is independently routable. Slice loss is local to that slice; reassembly state never corrupts a logical frame.
- L0/L1 zero-copy: a single DMA buffer per slice; refcount fan-out to N subscribers; no copies on the data path.

**What breaks:**

1. **Path resolution at 16k operations per second per publisher.** Each `write("/camera/frame[i]", ...)` resolves the path against the local graph. The path syntax is UTF-8 strings; resolution is a tree walk on every segment. For a vertex map with ~1000 paths this is ~few µs per resolution; at 16k writes/sec, this is ~1.5 % of one core. Tractable for one stream — disastrous if you have 8 cameras.

2. **Wildcard subscription cost.** A subscriber registered with `/camera/frame[*]:subscribers[]` is matched against every concrete write. The spec says the implementation MAY pre-cache the match set — if it does, fine. If it walks the wildcard table per write, this is O(N_wildcards) per write × 16k writes/sec.

3. **Dispatcher contention.** 16k dispatch ops per second per camera. With 8 cameras and 2 subscribers each, that's 256k dispatches per second. In the reference design, all dispatches go through one dispatcher; without sharding, the dispatcher's lock and queues become the bottleneck before the network does.

4. **Bridge dedup recent-set.** A bridge re-publishing this stream to a remote viewer must keep `(origin_peer_id, origin_timestamp, hop_count)` in its recent-set for the slice's whole delivery window. With 16k slices/sec and a 1-second window, that's 16k entries at any given time. With three streams, ~50k entries. With the documented sizing formula in [07-host-embedding.md](../reference/07-host-embedding.md) (`expected_max_fanout × delivery_window × publish_rate`), this swells fast. The recent-set is per-bridge, but a fleet-wide deployment compounds: each bridge holds its own.

5. **Recent-set hash hits and cache misses.** At ~50k entries lookup-on-every-incoming-bridged-TLV, the recent-set table overflows L2 cache on a typical Cortex-A. Every dedup check is a memory lookup; the bridge becomes memory-bandwidth-bound before it becomes network-bound.

6. **`opt.LL=0` (default u16 length) caps slices at 64 KiB.** Switching to `LL=1` (u32 length) goes to 4 GiB per slice, but each slice still walks the dispatcher individually. The fundamental cost isn't slice size — it's slice count.

7. **No batching / coalescing.** The protocol has no primitive to deliver "32 slices as one dispatch event." Each slice is a separate TLV by design. Coalescing inside one transport batch (TCP_NODELAY off, UDP packet packing) is at the transport's discretion; the L4 layer above sees individual TLVs.

**What the design needs to address:**

- **A documented path-interning convention.** Recommend: paths are interned at registration; the wire carries the path *string* (for routing); the local graph indexes by interned ID. Every implementation should mandate this.
- **A larger default slice size for streaming workloads.** 1 MiB or even 4 MiB cuts the slice count by an order of magnitude. The 64 KiB default came from the u16 length default; bump it for streams via `LL=1`.
- **A "frame" concept above the slice level.** An optional `manifest TLV` declaring "the next N slices belong to logical frame F" lets a router/dispatcher batch the dispatch.
- **Sharded dispatcher.** Multiple dispatcher threads keyed on path hash. Each shard owns a subset of the path namespace.
- **Recent-set replaced with bloom filter or counting bloom for high-rate streams** — false positives are tolerable (an accidental drop is OK), false negatives (missing a duplicate) are not. Bloom filters give O(1) bounded memory; the cost is tuning the false-positive rate to the loss tolerance.

### Workload B — high-rate sensor fan-in (100 sensors × 1 kHz)

- 100 sensors, each writing a 100-byte TLV at 1 kHz = 100k TLVs/sec total.
- Each TLV is small enough to fit in one MTU; no slicing.
- Each subscriber may be on a different transport (some local, some over TCP).

**What works:**

- Per-TLV header is small (4 bytes). Wire overhead is bounded.
- Each sensor publishes to a distinct path; no wildcard ambiguity unless explicitly aggregated.

**What breaks:**

1. **100k path resolutions per second.** Same problem as Workload A but distributed. Each must hit a different vertex. Without interning, path lookup dominates.
2. **100k subscriber-list iterations per second.** Each write walks `:subscribers[]` and dispatches. Per-vertex iteration cost is small, but if many subscribers are on slow transports (CAN, UART), the slow transport's send queue becomes the bottleneck. The protocol has no per-subscriber back-pressure to drop messages from the slow path *only*.
3. **No batching across writes from the same sensor.** A sensor publishing 1000 small samples per second is 1000 separate TLVs. Each gets a 4-byte header + 8-byte trailer = 12 bytes overhead per 100-byte payload = 12 % overhead. The structured-TLV mechanism could be used to batch (e.g., emit a `USER_SAMPLE_BATCH` of 10 samples once every 10 ms), but this is application-layer choice, not a protocol primitive.

**What the design needs:**

- A documented batching pattern (in `06-user-data-packing.md`) showing the structured-TLV-as-batch idiom and its tradeoffs.
- Per-subscriber asymmetric flow-control: a `:settings.subscribers[N].on_overrun = drop_oldest | drop_newest | block | exception` field, observable from the publisher.

### Workload C — 1 GB/s ADC into NCCL/RDMA

The reference suite mentions "1 GS/s ADC" as a stretch case. Let's be honest about what libtracer can actually do here.

- 1 GS/s × 4 bytes per sample = 4 GB/s.
- 4 GB/s / 64 KiB slices = 65k slices/sec.
- 4 GB/s / 1 MiB slices = ~4k slices/sec — more reasonable.

**The hard truth:**

- libtracer is **a control plane** at this rate, not a data plane. The actual bytes go through RDMA / NCCL / GPUDirect; libtracer carries the queue-pair handles, the GPU memory regions, and the metadata.
- The design admits this in [00-overview.md](../reference/00-overview.md) but the message gets diluted by other claims that imply libtracer can carry real data at GB/s. **Recommend**: section the `00-overview.md` "load-bearing claims" with an explicit table of what libtracer carries vs what it negotiates.

**What the architecture review wants from this scenario:**

A clear "control plane vs data plane" table showing:

| Workload | libtracer's role |
| ---- | ---- |
| 5-byte RC control @ 100 Hz | Full data plane. Trivial. |
| 1 KB sensor @ 1 kHz | Full data plane. |
| 100 KB / s sensor stream | Full data plane. |
| 30 MB/s 4K video | Full data plane on LAN; on CAN/UART it's the wrong tool. |
| 1 GB/s | Control plane only. RDMA / NCCL / SHM ring carries actual bytes. |
| 100 GB/s GPU-to-GPU | Control plane only. |

This already roughly exists in [00-vision-and-reality-check.md](00-vision-and-reality-check.md) but it's not tied back to specific protocol surfaces. The reference suite would benefit from making this explicit.

---

## Stress test 2 — Lowest-latency control loops

The README claims:

| Path | Realistic per-hop |
| ---- | ---- |
| In-process pub→sub via callback | ~200 ns – 1 µs |
| Inter-process via SHM | ~1–5 µs |
| LAN TCP, no congestion | ~50–200 µs |

Let's walk these.

### Scenario A — in-process pub→sub callback at 200 ns

To hit 200 ns end-to-end on a modern CPU, the publish path must:

1. **Resolve the path** — 50 ns at best with a hash-interned path; 1 µs+ for naïve string compare. **Spec doesn't mandate interning.**
2. **Bump the segment refcount** — 5-10 ns relaxed atomic.
3. **Build the TLV header** — 5-20 ns (header is small).
4. **Walk subscriber list** — 10-30 ns per subscriber for a contiguous array.
5. **Per subscriber: clone view (alloc + ref inc + struct fill) + enqueue** — 50-100 ns per subscriber.
6. **Subscriber wake** — condvar signal or futex wake — 100-300 ns.

The 200 ns figure assumes (a) interned paths, (b) one subscriber, (c) lock-free queue, (d) the subscriber is already spinning. Real-world: a single one of those misses puts you above 1 µs.

**What the spec doesn't say:**

- That paths must be interned for low-latency.
- That subscriber queues must be lock-free for sub-µs.
- That subscribers must be spinning (not blocked) for sub-µs.

These are deferred to "implementation choice" in [10-module-catalog.md](../reference/10-module-catalog.md), but the latency numbers in 00-overview.md presume them.

**Recommendation**: document the *latency contract*: "the protocol does not impose this latency; achieving it requires the implementation to (1) intern paths, (2) use lock-free subscriber queues, (3) spin or have hardware wakeup." Without this, the latency table in 00 sets expectations the spec cannot meet.

### Scenario B — refcount contention under fan-out

A vertex with 4 subscribers, each on its own CPU core, all decrementing the same segment's refcount on view release.

```
core 0: view_release(v0)  → atomic dec on segment refcount cache line
core 1: view_release(v1)  → same cache line
core 2: view_release(v2)  → same cache line
core 3: view_release(v3)  → same cache line
```

With 4 cores contending on the same cache line, each atomic operation costs 100-300 ns (cache-coherence round-trips). Sequential decrement: 4 × 200 ns = 800 ns just for refcount management.

For a 4-subscriber vertex updating at 1 MHz (1 µs period), refcount contention alone is 80 % of the time budget.

**Mitigation strategies the spec doesn't mention:**

- **RCU instead of refcount** for read-mostly segments. Frees the segment when the grace period elapses. Zero atomic on reader side. Cost: the writer must wait one grace period before reusing.
- **Sharded refcounts** — per-CPU refcount cells, summed at destruction. Fixes contention but complicates segment teardown.
- **Hierarchical refcounts** for large fan-out — N counts in a tree, sub-counts dec into parent only when sub-tree is done.

The reference C implementation will hit this immediately on multi-socket Linux servers under fan-out.

### Scenario C — SHM at 1-5 µs

The OPEN QUESTION on cross-process refcount in [10-module-catalog.md](../reference/10-module-catalog.md) §`mem_shared` defaults to "single-publisher, multi-reader." This means the cross-process zero-copy story is **publisher-owned**: readers are passive consumers; if the publisher dies, readers' views become stale.

For µs-class IPC between two equally-active processes (e.g., a renderer and a compositor), this falls back to copy-at-process-boundary (Option C). The spec admits this; the implication is that libtracer's SHM transport is a unidirectional pub/sub channel, **not** a true zero-copy shared workspace.

For comparison, Iceoryx2's robust shared refcount lets the SHM region survive any participant's death and supports multi-publisher. libtracer's design choice trades that capability for a smaller spec; the user pays in expressivity.

**Recommendation**: be explicit in 09-memory-substrate.md that `mem_shared` is "passive consumer" semantics; bidirectional active-active SHM is deferred to a future `mem_iceoryx2` module.

### Scenario D — TCP at 50-200 µs

Ordinary kernel-bypass-free TCP between two Linux hosts is 50-200 µs round-trip. libtracer adds:

- TLV serialization (header + trailer): ~50 ns (negligible).
- Path resolution at receiver: 100 ns – 1 µs.
- Subscriber dispatch: 100 ns – 1 µs.
- ROUTER attach at sender, ROUTER strip at receiver, dedup hit-test: 200-500 ns.

Net libtracer overhead per hop: ~1-2 µs on top of TCP's 50-200 µs. Reasonable. Not exceptional. For RDMA-class latency (sub-10-µs) the libtracer overhead is significant; you'd want the kernel-bypass transport to handle the data plane and use libtracer only for control.

---

## Architectural blockers

Each of these is something that prevents shipping libtracer as a general-purpose middleware. Tightly-scoped deployments may not hit them.

### Blocker 1 — Path resolution cost

**The problem.** Every read / write / subscribe operation involves a path string. The spec ([03-addressing.md](../reference/03-addressing.md)) defines the syntax but is silent on resolution mechanism. A naïve implementation parses and walks the path on every operation; at the latencies and rates the design claims, this is the dominant cost.

**When it hits.** Any deployment with > few thousand operations per second per host. Effectively all production deployments above the toy scale.

**Why it's not "just an implementation detail".** The reference C implementation will be benchmarked. Benchmarks will be cited. If the reference impl uses string compare, the benchmarks will be embarrassing. If it interns, then *interning* becomes part of the de facto spec — a spec written in C, not in the reference docs, and not portable.

**Fix.**
- Document an interning convention: path strings are interned to a 32-bit ID at registration; subsequent lookups go through a flat hash table.
- Specify the hash function used (FNV-1a 32-bit is sufficient).
- Note that wire-format paths remain UTF-8 strings; interning is local-only.
- Optionally: define a wire-format `PATH_HANDLE` type code (in the `0x0E – 0x7F` reserved range) that lets two peers negotiate a per-session integer alias for a path. Reduces per-message overhead from path-bytes to 4 bytes.

### Blocker 2 — Address-shift dispatch overhead

**The problem.** Slicing a 1 GB frame across `ep[0..N]` produces N independent TLVs. Each goes through path resolution, subscriber match, fan-out, and bridge dedup *individually*. The data plane is zero-copy; the **control plane is N times the cost**.

**When it hits.** Streaming workloads (video, ADC, lidar). The bandwidth claim survives; the messages-per-second claim collapses.

**Fix.**
- A `manifest` mechanism: one TLV announcing "I will write `/camera/frame[0..N]` at timestamp T; treat the group as one dispatch event." Subscribers register interest in the group; the dispatcher delivers slices to the same subscriber set without per-slice resolution.
- Equivalent at the bridge dedup layer: dedup by `(origin_peer_id, group_timestamp)` not `(origin_peer_id, slice_timestamp)`. One recent-set entry per group, not per slice.
- Larger default slice size for `LL=1` streams. 64 KiB is too small for hundreds of MB/s.

### Blocker 3 — Refcount fan-out contention

**The problem.** Single-counter atomic refcount on segments. With N subscribers fanning out from one publisher, refcount inc happens N times on one cache line; release happens N times on the same cache line. Contention scales linearly with fan-out.

**When it hits.** Multi-core fan-out at ≥ 100 kHz. Linux server with several subscribers on different cores reading a hot vertex.

**Fix.**
- Document an RCU / sharded-refcount fallback for read-mostly hot vertices. Implementation-level choice, but it must be in the latency contract.
- The Boost intrusive_ptr pattern is correct for *correctness*. It is not optimal for *contention*.

### Blocker 4 — Bridge dedup recent-set scaling

**The problem.** The recent-set sizing formula in 07-host-embedding.md scales as `fanout × window × rate`. For a 30 fps camera at 64 KiB slices, that's ~16k entries per stream per second of window. With several streams and multi-second windows, hundreds of thousands of entries. LRU eviction under bursts makes dedup briefly fail, which is when storms happen.

**When it hits.** Any streaming bridged deployment.

**Fix.**
- Bloom or counting-bloom recent-set with documented false-positive rate. Trade rare lost messages (false positive) for bounded memory.
- Hierarchical dedup: dedup on group identity, not slice identity, when the address-shift pattern is in use.
- Document the recent-set as a *flow-rate-bounded* resource and prescribe behavior when capacity is exceeded (drop on insert? lazy resize? stop forwarding?).

### Blocker 5 — No protocol-level flow control

**The problem.** The protocol has QoS (`reliability`, `deadline`, `keep_last_n`) but no native back-pressure signal. A slow subscriber consumes memory until something dies. Documented behavior on overflow: "implementation-defined."

**When it hits.** Any deployment with mixed-speed subscribers — mobile clients, slow disk recorders, slow wireless links.

**Fix.**
- Per-subscriber `:settings.on_overrun` field (`drop_oldest | drop_newest | block_publisher | exception`).
- Publisher introspection: `:settings.subscribers[N].queue_depth` and `:settings.subscribers[N].watermark` as readable fields.
- A standardized `STATUS=BACKPRESSURE` error code that subscribers may emit toward publishers (over the same path's `:status` channel).

---

## Architectural blind spots

These are areas the design doesn't cover at all. Each is a hole that needs filling before the design is "complete."

### Blind spot 1 — Security in v0.1

Security wraps are deferred to optional modules. A bare libtracer node has **no peer authentication**, no integrity beyond CRC (which protects against bit flips, not adversarial tampering), no confidentiality. The "decentralized graph spanning CAN+IP+RDMA" pitch is genuinely useful — but a graph that bridges a CAN bus into a public IP network is, by default, a security disaster.

**Concrete failures:**

- A malicious peer on the LAN announces itself via mDNS with a peer-id and immediately receives traffic.
- A bridge with two transports forwards CAN messages onto the IP transport with no authentication on the IP side.
- The user-range type code space (`0x80-0xFF`) lets any peer push opaque payloads at any subscriber.

**Fix scope (none of this is v0.1):**

- A `security_required = true` flag at the node level: if true, every transport must be wrapped by a security module before any TLV is accepted.
- A peer-id-to-public-key binding mechanism (TOFU, CA-signed, or pre-shared).
- An ACL evaluation step at the bridge boundary, *before* the dedup recent-set is consulted (so unauthorized writes don't pollute dedup state).

### Blind spot 2 — Schema versioning and evolution

[02-graph-model.md](../reference/02-graph-model.md) defines schemas as a TLV at `:schema`. There is no story for:

- A producer adds a new field to its schema; existing subscribers have cached the old schema. Behavior?
- A producer reorders fields. The wire format is fine, but field-path resolution against the new schema may surprise the subscriber.
- A producer's schema is *different* between two libtracer versions (Mode A canvas vs Mode B canvas) but the path is the same.

**Fix.**

- A `:schema_version` integer at every vertex; subscribers cache by `(path, schema_version)`. Schema change bumps the version.
- A `STATUS=SCHEMA_CHANGED` event delivered to subscribers when the cached version goes stale.
- A "compatibility window" for in-flight TLVs: any TLV produced under schema_version N is interpretable through N or N-1; older versions are dropped at the bridge with a STATUS event.

### Blind spot 3 — Per-transport multiplexing

The transport ABI in [10-module-catalog.md](../reference/10-module-catalog.md) `transport_vtable_t` accepts a TLV and a peer-id but doesn't say:

- Does each peer-id pair get one TCP connection? Or one per topic? Or one per priority class?
- How does a transport multiplex concurrent reads / writes / subscriptions on the same socket?
- What's the head-of-line blocking story for high-priority small messages behind low-priority large frames?

The current design pushes this to the transport implementation. For TCP that's fine ("one connection per peer-pair, priorities preserved by send order"). For QUIC that under-uses the protocol (QUIC streams give priority + multiplexing for free). For UDP it's irrelevant.

**Fix.**

- An optional `:settings.transport.<name>.priority` field per subscriber; the transport decides what to do with it.
- A documented transport convention: "one connection per peer-pair; priority preserved by send-order" as the v0.1 baseline; transports are free to do better.

### Blind spot 4 — Lifecycle management

When does a vertex go away? The spec lets you *create* vertices but is silent on:

- A vertex with no subscribers and no recent writes: garbage-collected? Stays forever? Configurable?
- A peer disconnects: its bridge proxy vertices on remote hosts — kept? Reaped? After how long?
- A subscriber's deadline-expired: removed from the subscriber list? Marked dead?
- A schema goes stale: new vertex re-registered with the same path; do old subscribers re-bind?

**Fix.** A `lifecycle_policy` per vertex: `permanent | reap_after_idle_seconds | tied_to_peer`.

### Blind spot 5 — Diagnostics and observability

`tracer-top` is mentioned in the roadmap, but no architectural support for:

- Distributed tracing (OpenTelemetry-style) that follows a write across N bridges.
- Per-edge sample-rate metrics.
- Per-subscriber latency histograms.
- A "graph health" snapshot suitable for an SRE dashboard.

The protocol surface (write a STATUS to a `/diag/...` path) is sufficient to *implement* these, but the design doesn't standardize the paths. Two implementations would surface diagnostics differently.

**Fix.** A standard `:metrics.*` namespace at every vertex, with mandatory fields (`writes_total`, `bytes_total`, `subscribers_count`, `last_write_ns`, `errors_by_code[]`).

### Blind spot 6 — Determinism / replay

A recorder records the byte stream. Replay reconstructs writes in original order with original timestamps. But:

- The protocol's "last-write-wins by timestamp" is **last-arrived-wins-by-arrival-order** in practice if timestamps are equal or unsynced.
- A replay against a graph in a different state (e.g., different vertex topology) is not faithful.
- Vertex-side state (sink-with-model) is not captured by recording the writes; a model that diverges across replays cannot be reconstructed from the recording alone.

**Fix.** Out of scope for v0.1; document this as a known non-goal.

### Blind spot 7 — Cross-vertex transactions

Atomic multi-field write on a single vertex is supported (by writing a structured TLV to the parent path). Atomic across two vertices is not. A user that needs "either both motors get the new setpoint or neither does" must build the protocol on top.

**Fix.** Out of scope for v0.1. If it ever becomes scope, look at two-phase commit or, more likely, push the user toward modeling it as one compound vertex.

### Blind spot 8 — Time and clock skew

Wire-trailer TS is a u64 ns since Unix epoch. The protocol assumes:

- All hosts have synchronized clocks (PTP, NTP, GPS).
- Skew within ±2 seconds (the i32 relative-TS range).
- Monotonicity within a peer.

In practice, a fleet of 1000 ESP32s has clocks that drift by tens of milliseconds. A multi-host system with no PTP and only NTP has correlated skew of ~5 ms typically. Last-write-wins with this much skew **is wrong** for control-plane correctness.

The spec mentions this in 04-communication-flows.md §coherency notes ("ms accuracy without HW PTP") but the implication is buried.

**Fix.** Loud, explicit warning in [04-communication-flows.md](../reference/04-communication-flows.md): "Without HW PTP, last-write-wins by TS is not a correctness primitive — it is a tiebreaker. Application logic that needs ordered writes must implement application-level sequencing."

---

## OPEN QUESTIONS that block production use

These are explicitly listed in [10-module-catalog.md](../reference/10-module-catalog.md) §hard integrations and [08-views-and-ownership.md](../reference/08-views-and-ownership.md) §OPEN QUESTIONS. Re-summarized here with prioritization.

| OPEN QUESTION | Severity | When it blocks |
| ---- | ---- | ---- |
| Cross-process refcount on `mem_shared` | High | First time anyone wants bidirectional zero-copy IPC |
| MMIO TOCTOU | Medium | First time someone wires up a register-as-vertex naïvely |
| Boost asio streambuf | Low | Only if a C++ user tries to integrate; default is "don't" |
| lwIP pbuf aliasing | High | Every Wi-Fi MCU deployment |
| Rope walk vs flatten | Medium | Every transport that doesn't support scatter-gather |
| DMA cache coherency | High | Every non-cache-coherent SoC (most Cortex-M) |
| Reference-to-a-live-value | Low | Quality-of-life convenience helper |

Two of these (lwIP pbuf, DMA cache coherency) are universal in the embedded world. They need crisp resolution before any embedded shipping; the recommendations in the OPEN QUESTION sections look right but are not yet specified in the layer docs themselves.

---

## Comparison to peer technologies — where libtracer wins, where it loses

| Capability | libtracer | Zenoh | Iceoryx2 | DDS (Cyclone, Fast) | NATS |
| ---- | ---- | ---- | ---- | ---- | ---- |
| Modular core ≤ 16 KB | targeting | no (~100 KB) | no (~50 KB) | no (~MB) | no (~KB but flat) |
| Same-substrate (memory == wire == graph) | yes (novel) | no | partial (SHM only) | no | no |
| Read/write/subscribe unified API | yes | partial | no | no | no |
| Address-shift slicing instead of fragmentation | yes (novel) | no | no | no | no |
| Bridges in core | yes | optional | no | no | no |
| Wire format frozen v1 | committed | evolving | partial | frozen | frozen |
| MCU support | targeting Cortex-M3+ | yes (zenoh-pico) | no | no | no |
| Multi-language | C / C++ / Rust later | many | C++ / Rust | many | many |
| ROS bridge | no | yes | no | yes | partial |
| Discovery | mDNS + static | gossip + mDNS | static | DDS-RTPS | NATS-builtin |
| Security | optional, deferred | optional | optional | mandatory in conformance | TLS-only |
| QoS | 5 policies | many | few | 22 policies | none |
| Active maintainer count | 1 | ~10s (Eclipse) | ~5 | ~10s | ~10s |
| Maturity | v0.1 design | 1.0+ | 0.5+ | 5+ years per impl | 2.0+ |
| Ecosystem (tooling, drivers) | none yet | rich | growing | rich | rich |

**Where libtracer wins:**

- Smallest binary on MCU class hardware, *if* the 16 KB sentinel holds in implementation.
- Single API for reads, writes, events, subscriptions — real ergonomics improvement.
- Same-substrate insight is genuinely novel. Two competing libraries (Iceoryx, FlatBuffers) achieve some of this, but neither has the graph-routing / wire-format / memory unification all at once.
- Address-shift slicing is a clean way to remove a class of bugs from the protocol surface.
- Spans multiple buses (CAN + IP + SHM) without a separate bridge product.

**Where libtracer loses:**

- No production usage. Every design choice is theoretical until benched.
- No ecosystem. No ROS bridge. No MATLAB / Simulink integration. No language bindings beyond what's planned.
- Smaller QoS surface than DDS — production users that need 22 policies will have to extend.
- Single-maintainer bus factor.
- Security story is post-MVP; "decentralized graph" without baked-in security is a serious operator risk.
- Performance promises (sub-µs in-process, 1 GB/s with zero-copy) are achievable in principle but require implementation choices not in the spec — until reference benchmarks exist on real hardware, claims are unverified.

**Net verdict.** libtracer is a credible, opinionated, intellectually clean alternative for *embedded and small-distributed* systems. It is not a credible Zenoh / DDS replacement for general-purpose middleware in 2026. The differentiators (same-substrate, modular ≤16 KB, unified API) are real but narrow.

---

## Risk-prioritized fixlist

Sorted by what blocks shipping a v0.1 anyone other than the maintainer would adopt.

### P0 — must address before v0.1 freeze

1. ~~**Document path interning convention**~~ **DONE 2026-05-06** — addressed via static path handles in [../spec/v1.md](../spec/v1.md) §3.1, [../reference/03-addressing.md](../reference/03-addressing.md) §static path handles, and the dispatch-keyed-on-PATH-TLV-bytes rule in [../reference/02-graph-model.md](../reference/02-graph-model.md). Stronger than the original ask: not just interning convention but a normative MUST.
2. **Document the latency contract** (1 paragraph in 00-overview.md plus an appendix listing the assumptions: handle dispatch, lock-free queues, spinning subscribers).
3. **Document the manifest pattern for address-shift groups** to amortize dispatch and dedup cost.
4. **Resolve and specify lwIP pbuf and DMA cache OPEN QUESTIONS** in 09-memory-substrate.md, not just listed as open.
5. **Specify per-subscriber overflow behavior** (`:settings.on_overrun`).
6. **Loud warning about clock skew and last-write-wins** in 04.

### P1 — should address before v0.1 ships

7. Sharded / RCU refcount option documented in 08-views-and-ownership.md.
8. Schema versioning convention documented in 02-graph-model.md.
9. Standard `:metrics.*` namespace documented in 02 or 04.
10. Bloom-filter recent-set option documented in 07.
11. Larger default slice size guidance for streaming workloads in 03.
12. Lifecycle policy field documented in 02.

### P2 — can address post-v0.1 with reference impl

13. PATH_HANDLE wire-level type code for path interning negotiation (would consume one slot in the `0x0E-0x7F` reserved range).
14. Hierarchical bridge dedup keyed on group identity.
15. Per-transport multiplexing convention.
16. Standard distributed-trace TLV.

### P3 — explicit non-goals; document as such

17. Cluster consensus / CRDTs.
18. Cross-vertex atomic transactions.
19. Sub-µs cross-process IPC (defer to iceoryx2 or similar; document libtracer as control plane only).
20. ROS / DDS bridge (could be a future module; not v0.1).

---

## What this review is NOT

- Not a re-design proposal. The architectural review surfaces issues; the maintainer decides scope.
- Not a benchmark report. Numbers cited are order-of-magnitude estimates from public sources (Iceoryx2 internals, lwIP performance studies, Linux atomic-contention papers). Real numbers come from the week-8 benchmark milestone.
- Not a rewrite of the design. Most identified gaps are *additive*: text in existing docs, new fields in existing schemas, new modules that fit the existing layer model. The architecture itself does not need to change.
- Not a vote of confidence or no-confidence. The verdict is conditional: ship v0.1 for the embedded-and-small-distributed market with clear scope; do not market against Zenoh / DDS for general middleware until P0 + P1 above are addressed and reference benchmarks exist.

---

## Reading guide for this review

If you want to make a decision in 10 minutes: read **§executive verdict** and **§risk-prioritized fixlist**.

If you want to know whether the design holds up under streaming workloads: read **§stress test 1**.

If you want to know whether the latency claims are real: read **§stress test 2**.

If you want to know what the spec doesn't cover at all: read **§architectural blind spots**.

If you want to know how libtracer stacks against alternatives: read **§comparison to peer technologies**.

If you are the maintainer and need a checklist: read **§risk-prioritized fixlist** P0/P1.
