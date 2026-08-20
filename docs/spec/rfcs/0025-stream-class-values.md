<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0025 — Stream-class values: delivery classes over the rope primitive

| Field | Value |
| ---- | ---- |
| **RFC** | 0025 |
| **Title** | Stream-class values: delivery classes over the rope primitive |
| **Status** | **accepted** (opened 2026-08-12, accepted 2026-08-12 by the merge of [PR #1192](https://github.com/avatarsd-llc/libtracer/pull/1192); implementation tracked by [#1204](https://github.com/avatarsd-llc/libtracer/issues/1204)). The comment window is **waived by default** per [GOVERNANCE](../../../.github/GOVERNANCE.md) single-maintainer rule — invoked explicitly only if outside input is wanted, and it was not invoked here. This line previously read `in-comment`, which under the waived-by-default rule is a state the RFC never occupied. **Amendment 1 (2026-08-21, §4.2.1) replaces §4.2's single-clock timestamp spelling with the three-clock model — wire/TX on the trailer (outermost frame, always `TF=0`), SAMPLE in a payload `TIME` (`0x0C`) child, PLAYOUT receiver-derived — and re-shapes the batch accordingly: a `TIME{u64 base}` child, per-sample time derived at **0 bytes/sample** for a uniform stream, a packed `i32` offset array for a non-uniform one. **Amendment 2 (2026-08-21, §4.6.1) relocates the ring: a producer never queues — the queue belongs to whoever consumes it, materializes at the *receiving* vertex as a STREAM, and is bounded in BYTES by that vertex's own injected `mem::block_source_t`; §4.4's pressure contract binds at the receiver ring, cross-writer total order is delegated to ADR-0019 HLC stamps, and the per-edge credit window is PARKED as the v2 wire-level escalation. Both amendments transcribe the 2026-08-20 grilling rulings (Q8, Q9, Q10) and neither adds a wire byte. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-08-12 |
| **Tracking issue** | [#879](https://github.com/avatarsd-llc/libtracer/issues/879) (folds [#863](https://github.com/avatarsd-llc/libtracer/issues/863), per-subscription delivery QoS) |
| **Supersedes** | the in-comment draft of [PR #893](https://github.com/avatarsd-llc/libtracer/pull/893) (see §9 — its three forks resolved against the draft's side in the 2026-08-12 grilling rulings) |
| **Extends** | [RFC-0022](0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.A (the packed `delivery_policy` u16) |
| **Target spec version** | v1 (draft refinement — v1 is unreleased, so no v2 is needed) |

## 1. Summary

A vertex value today is an **LKV** — a last-known value: writes replace, delivery may
conflate, and a read answers "what is it now?". A *signal* — audio, video, an ADC
capture — answers "what happened, in order?", and LKV semantics mis-serve it
structurally: samples are not idempotent, order and completeness carry the meaning,
and conflation is data loss without a receipt.

This RFC adds the signal plane as a **delivery policy over the primitive the
protocol already has**, with **zero new wire grammar**:

- The **rope is the one storage primitive** (ADR-0053); the LKV is its degenerate
  one-link case. There is no "stream type" beside an "LKV type".
- A **batch is a convention over the existing format**: a structured (`opt.PL=1`)
  written value whose children are the sample frames, timestamped with the trailer
  forms [01-data-format.md](../../reference/01-data-format.md) already specifies —
  an absolute `TF=0` u64 base on the parent, a compact `TF=1` i32 per-child offset.
  No new TLV type, no new `opt` bit, no header layout. **Amendment 1 (§4.2.1)
  supersedes that timestamp spelling**: the trailer carries wire/TX time only, and a
  batch's sample time is a payload `TIME{u64 base}` child — 0 bytes/sample on a
  uniform stream, a packed `i32` offset array on a non-uniform one. Still zero new
  grammar.
- The **stream descriptor is a separate LKV vertex** beside the data vertex —
  declared once, never repeated per batch, browseable by every existing tool.
- The QoS folded in from #863 becomes a **delivery-class field in bits 6–7 of
  RFC-0022's packed `delivery_policy` u16** — `0` conflate (default, bit-compatible
  with today), `1` immediate, `2` batch, `3` stream — declared **per subscription**,
  enforced at the producer's fan-out edge. No per-vertex "I am a stream" flag exists:
  the plane is **demand-driven and init-free**, so load-bearing claim 5 (the graph
  imposes no shape on user data) stays intact.
- Under pressure, behaviour is the **existing `reliability` bits × the class**
  (§4.4): best-effort sheds oldest **with an explicit gap signal**
  (`tr::flow::address_shift_gap`, generalized by §4.5 to "a discontinuity in an
  ordered flow"); reliable surfaces `tr::flow::backpressure` at the producer.
  Every loss is accounted; silence is non-conforming.
- The stream ring is **PMR-backed** — bounded by the injected resource, never a
  magic number — and reconciles with RFC-0022 §8 by composition (§4.6).
  **Amendment 2 (§4.6.1) relocates that ring**: a producer never queues. The queue
  belongs to whoever consumes it — it materializes at the **receiving** vertex,
  bounded in **bytes** by that vertex's **own** injected `mem::block_source_t`, never
  a shared pool.
- At extreme rates the graph is a **control plane only** (§4.7): the descriptor
  negotiates an out-of-band channel, carries credentials **by reference only**, and
  the graph never carries the sample bytes.

Implementation lands in three independently useful phases (§5); phase 1 is the
timestamp-plumbing work already tracked as
[#1109](https://github.com/avatarsd-llc/libtracer/issues/1109).

## 2. Motivation

1. **LKV + conflation is the wrong contract for signals.** Delivery selection is
   deliberately structural and value-agnostic (RFC-0008), storage is
   replace-in-place, and RFC-0022 tunes *which last value* a subscriber sees — every
   existing axis optimises "current state"; none expresses "complete ordered history
   at rate". A signal consumer needs ordering, gap visibility, and a
   latency/completeness trade that the *subscriber* chooses.

2. **Downstream invented batch streaming twice, divergently.** A downstream firmware
   (ESP32-C6) ships two hand-rolled stream conventions over opaque `VALUE` payloads:
   a 2 kHz ADC scope emitting `{u32 t0_us, u16 dt_us, u16 n}` sample batches, and a
   PWM timeline `{ver, nch, dt_us, t0_us, nframes}` with receiver-side clock-offset
   EWMA, a jitter buffer, and epoch reset on staleness. Both re-solved framing,
   timing, and loss handling; neither gets protocol-level loss accounting or reuse.
   Duplication downstream is the classic signal that a primitive is missing one
   layer up. This RFC generalizes those shapes; it does not invent new ones.

3. **The rate envelope spans six orders of magnitude.** ~1 Msps on an ESP32-class
   node (1 Msps × 12-bit packed ≈ 1.5 MB/s — at the C6 WebSocket ceiling, within
   UDP reach) up to 1–100 Gsps between hosts. One model must serve both ends
   *without* pretending they share a data plane — hence the §4.7 tiering, where the
   graph never routes the extreme tier's bytes.

4. **#863 and #879 are one design.** "Stream value" is not a vertex type — it is a
   delivery policy, which is exactly the per-subscription QoS #863 proposed. This
   RFC is the single document covering both.

## 3. Design tenets (the ruled trunk)

Settled across the 2026-08-06 direction, the 2026-08-06 grill synthesis, and the
2026-08-12 dedicated grilling session on #879; recorded here as the normative frame,
not re-opened:

- **One storage primitive.** A vertex value is a rope; the LKV is the degenerate
  one-link rope. LKV = *conflating delivery*; stream = *append-preserving delivery*.
  The distinction lives on the **subscription**, enforced at the producer's fan-out
  edge — never as a vertex-init flag.
- **Demand-driven, init-free.** A vertex behaves as an LKV until a subscription's
  delivery class says otherwise. No app-side declaration; claim 5 holds.
- **Zero new grammar.** No new type code, no new `opt` bit, no new framing. The wire
  is already the rope; the batch and its timestamps are conventions over shapes the
  format fully specifies and every conforming decoder already decodes.
- **A single value write is the degenerate one-frame batch.** Same op, same vertex,
  same ACL, same subscription surface — hot-path `set value` code needs no mode
  switch.
- **The rope is the zero-copy / RDMA path.** A large sample buffer maps as a rope
  over DMA'd segments (`mem_backend_t` cache hooks, ADR-0041/0042 one-copy
  discipline, ADR-0053 rope forwarding without flattening). Nothing stream-specific
  is added to the memory substrate.
- **No synthetic limits.** Every magnitude is full-width and every bound is an
  injected resource or per-target configuration (RFC-0006, RFC-0022 §3.A).
- **Producer owns cadence.** No rate caps, throttles, dirty tracking, or scheduling
  in libtracer (the standing Branch-write rule); batching cadence is the producer's,
  jitter-buffering is the consumer's.

## 4. Proposed change

### 4.1 Delivery class — bits 6–7 of the packed `delivery_policy` u16

RFC-0022 §3.A's packed u16 (carried under the existing `delivery_policy` key of the
SUBSCRIBER's `qos_settings` SETTINGS child) gains one two-bit field. The full layout
becomes:

| bits | field | values |
| ---: | --- | --- |
| 0–1 | `reliability` | `0` = best-effort, `1` = reliable; `2`–`3` reserved |
| 2–4 | `priority` | `0`–`7`, `0` = default |
| 5 | `durability_request` | `1` = deliver the producer's latched last value on join |
| 6–7 | `delivery_class` | `0` = **conflate** (default), `1` = **immediate**, `2` = **batch**, `3` = **stream** |
| 8–15 | reserved | MUST be written `0`, MUST be ignored on read |

Absent ⇒ all-zero ⇒ conflate ⇒ **today's LKV behaviour, byte-identically** — a
subscriber that predates this RFC wrote `0` in bits 6–7 (reserved, carried
verbatim), so old subscribers are bit-compatible **by construction**, and the
existing `subscriber/policy-absent` and `subscriber/policy-reserved-bits` vectors
keep their meaning with the reserved range narrowed to 8–15.

Class semantics, enforced at the producer's fan-out edge:

- **`0` conflate** — last-wins. Delivery MAY coalesce to the newest value; a
  subscriber lagging its producer observes the latest state, not the history. This
  is the LKV contract and the default.
- **`1` immediate** — every write is delivered as its own event, lowest latency,
  no producer-side accumulation. Order-preserving; never conflated.
- **`2` batch** — the producer's fan-out edge accumulates deliveries for this
  subscriber and flushes a §4.2 batch after **N sample frames or a T-window**,
  whichever the subscription configured (magnitudes in §4.1.1). Order-preserving;
  never conflated.
- **`3` stream** — append-preserving: every write is delivered, in order, none
  conflated, with the §4.4 pressure contract governing overload. This is the
  signal-plane class; it is what makes the existing STREAM-role drain machinery
  demand-driven rather than a storage-side special case.

#### 4.1.1 Magnitudes — full-width, in the subscription's cold half, keyed by class

Per RFC-0022 §3.A no magnitude is packed into the u16. The class magnitudes are
full-width `qos_settings` keys (absent ⇒ implementation default), stored in the
subscription edge's cold half and meaningful only under their class:

```
qos_settings += SETTINGS {
  NAME "batch_count"      VALUE <u32>   ; class 2 — flush after N sample frames
  NAME "batch_window_ns"  VALUE <u64>   ; class 2 — flush after T elapsed
  NAME "stream_depth"     VALUE <u32>   ; class 3 — RETIRED by Amendment 2 (§4.6.1):
                                        ;   depth is the CONSUMER's, sized in bytes
                                        ;   by its own injected source, never
                                        ;   requested from the producer. Carried
                                        ;   verbatim and ignored if present.
}
```

A key carried under a class that does not consume it is ignored (the standing
absent-⇒-default doctrine, applied in reverse). The `stream_qos` and
`stream_window` keys of the superseded #893 draft **do not exist**: the pressure
behaviour they selected rides `reliability` × class (§4.4), and the window
magnitude is `stream_depth` above.

### 4.2 Wire shape — the batch convention (zero new grammar)

A **batch** is a structured (`opt.PL=1`) written value whose children are the
**sample frames** — what the tracking issue's discussion called a
"LIST-of-sample-frames", spelled in the canonical vocabulary: there is **no LIST
type and no `0x05`**; the batch is an ordinary `PL=1` TLV with homogeneous children
(the ADR-0008 array shape), written to an ordinary vertex. The graph does not
interpret it (claim 5); the §4.3 descriptor tells consumers how to.

Timing rides the **existing optional trailer**, exactly as
[01-data-format.md](../../reference/01-data-format.md) §options bitfield specifies:

- the **parent** batch TLV carries `opt.TS=1, TF=0` — the absolute base timestamp,
  u64 LE ns;
- **each child** sample frame carries `opt.TS=1, TF=1` — a signed i32 LE ns offset
  **relative to the parent's timestamp**.

Both forms are already normative and already decodable by every conforming
receiver; this RFC adds zero grammar and zero registry entries. A **single value
write is the degenerate one-frame case** (trailer optional) — same op, same ACL,
same subscription surface.

Receivers interpret the timestamps as **scheduled playout time**; the
jitter-buffer / latency offset is receiver-side configuration (§4.7), set
separately from the stream. `opt.TS` here is the application-visible per-frame
stamp of ADR-0019's per-producer monotonic doctrine — nothing in the batch
convention introduces a second time domain.

**Producer-path prerequisites (phase 1, tracked as #1109).** Three reference-
implementation gaps must close before a stamp set at the origin survives the wire:
(1) a writer-side API sets `opt.TS` + the trailer value from an **injected clock**
(no ambient clock reads on the hot path); (2) the FWD/emit-cursor builders learn to
append trailer bytes instead of hardcoding the opt byte
(`core/src/op_resolve_walk.hpp`, `core/src/fwd_frame_view.hpp`); (3) the reply path
echoes or re-stamps rather than unconditionally `without_trailer()`-stripping —
scoped so a trailer-sliced copy stays self-consistent (the documented invariant in
`op_resolve_walk.hpp` is kept, not removed). These fixes are independently useful
(RTT/latency measurement over FWD) and gate nothing else in this RFC.

#### 4.2.1 Amendment 1 (2026-08-21, ruled) — three clocks, and the batch's sample time is a payload `TIME` child

**Instrument.** Amendment, not erratum: §4.2's batch spelling above is *replaced*, not clarified,
and the replacement changes which TLV carries a sample's time. No new type code and no new `opt`
bit is minted — `TIME` (`0x0C`) and the trailer both already exist and both are already normative
— but a conforming producer that followed §4.2 as written emits different bytes after this
amendment, which is amendment territory rather than erratum territory. Ruled by the maintainer in
the 2026-08-20 grilling session (Q9, Q10), against the implemented reality recorded in
[01-data-format.md](../../reference/01-data-format.md) §"Writer-side status (#1109)". The comment
window is waived by default under [GOVERNANCE.md](../../../.github/GOVERNANCE.md)'s solo-maintainer
clause and is not invoked.

**What this replaces.** Until this amendment §4.2 read:

> ~~the **parent** batch TLV carries `opt.TS=1, TF=0` — the absolute base timestamp, u64 LE ns;
> **each child** sample frame carries `opt.TS=1, TF=1` — a signed i32 LE ns offset relative to the
> parent's timestamp. […] Receivers interpret the timestamps as **scheduled playout time**.~~

That spelling conflated three distinct clocks onto one carrier and spent 4 trailer bytes per
sample to do it. It is retired in full. The trailer keeps exactly one job.

##### The three clocks

There are **three** times in a stream-class delivery, they are not interchangeable, and each has
exactly one carrier:

| clock | what it means | where it lives | stamped by |
| --- | --- | --- | --- |
| **WIRE / TX** | when this frame left an interface | the **optional trailer** (`opt.TS=1`), on the **OUTERMOST frame only**, **always `TF=0`** (absolute u64 LE ns) | the sender, at interface transmit |
| **SAMPLE** | when the datum was acquired | a payload **`TIME` (`0x0C`)** TLV **inside the value** | the producer, at acquisition |
| **PLAYOUT** | when the consumer should present it | **nowhere on the wire** — derived by the receiver | the receiver, from its own RTT/offset estimate |

Consequences, each of which is a MUST unless marked otherwise:

1. **The trailer TS is wire/TX time and nothing else.** It is stamped at interface transmit, on
   the outermost frame only, and is **always `TF=0`**. A relay that rebuilds a frame re-stamps its
   own outermost trailer (the FWD hop already re-emits it verbatim today; re-stamping is the
   sender's own transmit time and is the conforming reading of "wire time"). Inner TLVs of a
   structured value carry no trailer TS in this model. This is what the reference codec already
   does — "**TF=0 only, on purpose**" in 01-data-format.md's writer-side status is now the
   *design*, not a temporary gate.
2. **SAMPLE time is a payload `TIME` (`0x0C`) TLV inside the value**, exactly the
   application-domain use [05-protocol-tlvs.md](../../reference/05-protocol-tlvs.md) §`0x0C`
   already names ("sample-acquisition time"). The graph never interprets it (claim 5); the §4.3
   descriptor tells the consumer how to read it.
3. **PLAYOUT time is never transmitted.** It is receiver-derived from the RTT and clock offset the
   receiver estimates off the **read/write carrier echoes** — the terminus's TF=0 stamp echo on a
   reply, `RTT = origin_now − echoed_stamp`, computed entirely on the origin's clock with no
   request id, no clock sync, and no per-request state. The jitter-buffer / latency offset stays
   receiver-side library policy (§4.7), never a router duty and never a wire field. §4.2's
   sentence "receivers interpret the timestamps as **scheduled playout time**" was wrong on both
   counts and is withdrawn: what arrives is a sample time, and playout is the receiver's own
   arithmetic on top of it.
4. **`TF=1` is RESERVED grammar, not removed** (Q10). A decoder MUST still record the relative flag
   and its delta and succeed; a relay MUST carry a `TF=1` frame verbatim; the reply-echo path
   MUST decline a `TF=1` root (it has no anchor to echo against); and the reference writer stays
   **gated** — it does not mint `TF=1` until the anchorless-reject rule of
   [01-data-format.md](../../reference/01-data-format.md) is enforced where the stamp is
   **consumed**. `TF=1` is additive future surface this RFC does not use, not surface this RFC
   deletes.
5. **The ordering substrate is ADR-0019**, not any of the three clocks. Per-producer monotonic HLC
   stamps are what give a cross-writer total order
   ([ADR-0019](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0019-per-producer-monotonic-origin-timestamp.md));
   wire time is advisory, sample time is application-domain, and neither is a sequence number.

##### The batch shape, restated

A batch remains a structured (`opt.PL=1`) written value whose children are the sample frames
(§4.2's zero-new-grammar tenet is untouched). Its timing is now:

```
BATCH  (opt.PL=1, user-range record type)
  ├─ TIME  <u64 LE ns>          ; the batch BASE — sample time of frame 0
  ├─ [ VALUE <i32[] LE ns> ]    ; NON-UNIFORM streams only: packed per-sample
  │                             ;   offsets from base, one i32 per frame
  └─ <sample frames…>           ; homogeneous children, ADR-0008 array shape
```

- **A `TIME{u64 base}` child carries the batch's base sample time.** One per batch, not one per
  sample.
- **A UNIFORM stream spends 0 bytes per sample on time.** Per-sample time is *derived*:
  `t(i) = base + i × dt_ns`, where `dt_ns` is the §4.3 descriptor's nominal sample period. A
  uniform stream is exactly one whose descriptor declares `dt_ns != 0`. Nothing per-sample is
  transmitted, and this is the recommended shape for every regular acquisition — an ADC scope, a
  PWM timeline, a fixed-rate capture.
- **A NON-UNIFORM stream (`dt_ns == 0`) carries a packed `i32` offset array**: one signed i32 LE ns
  offset from `base` per sample frame, in one child, in frame order — **not** one trailer per
  child. 4 bytes per sample in one contiguous run, decodable in one span, with no per-child TLV
  header and no anchor walk.
- **A single value write remains the degenerate one-frame case** — a `TIME` child is optional on
  it, and its absence means "no acquisition time is claimed", never epoch zero.

The §7 vector `stream/batch-trailer-roundtrip` is restated by this amendment as
`stream/batch-time-roundtrip` (§7).

### 4.3 The descriptor — a separate LKV beside the data vertex

Sample rate, sample format, and channel count belong in a **descriptor**: a
`SETTINGS` value held by a **separate LKV vertex beside the data vertex** (e.g.
`/adc0` data, `/adc0/desc` descriptor — naming is the application's, per the naming-
authority doctrine), declared or negotiated once and never repeated per batch.

```
SETTINGS {
  NAME "format"    VALUE <u16>    ; sample format id (application-defined space;
                                  ;   the graph does not transcode)
  NAME "channels"  VALUE <u8>     ; sample frames are channel-interleaved
  NAME "dt_ns"     VALUE <u64>    ; nominal sample period; 0 = irregular
                                  ;   (per-frame trailer offsets carry all timing)
  NAME "channel"   SETTINGS {…}   ; optional — T2 out-of-band binding (§4.7)
}
```

Ruled consequences:

- The descriptor is **not** the data vertex's stored value, and there is **no
  per-vertex stream flag**. The data vertex is an ordinary vertex: its stored value
  is the last written rope (the latest batch or single value), `read` answers it,
  and the RFC-0022 durability latch replays it. Nothing about the vertex changes
  when a stream-class subscriber attaches — the plane is demand-driven.
- Because the descriptor is an ordinary vertex, browse, `:schema`, dumps, ACLs,
  subscriptions and the durability latch all work on it **for free** — a T2
  producer updating `channel.state` is making an ordinary low-rate value write that
  existing dashboards observe with zero new machinery.
- Descriptor content above is a **recommended convention**, not protocol grammar:
  the graph never validates it (claim 5). The reference library ships helpers that
  read and write this shape.

### 4.4 The pressure contract — `reliability` × class, no new knob

> **Amended 2026-08-21 by Amendment 2 (§4.6.1).** Everything this section says about
> *what* happens under pressure stands verbatim. *Where* it binds moves: the ring is
> the **receiver's**, not the producer's, so read "a producer's fan-out edge cannot
> enqueue" below as "**the receiving vertex's STREAM ring cannot admit**", and read
> the reliable arm's `FLOW_BACKPRESSURE` as travelling **back to the rate-aware
> producer**, which slows. The table's two contracts are unchanged.

When a producer's fan-out edge cannot enqueue a delivery for a subscriber (ring
full, resource exhausted), behaviour is selected by the subscription's **existing
`reliability` bits**, uniformly for the immediate, batch and stream classes:

| `reliability` | contract |
| --- | --- |
| `0` best-effort | **drop-oldest with a gap signal**: the implementation MUST shed the *oldest* queued delivery (whole, never partial), MUST account the loss, and MUST surface `tr::flow::address_shift_gap` (§4.5) to the subscriber in-order at the shed point. Latency stays bounded by the ring; completeness is sacrificed knowingly. *(prose name: the realtime-lossy behaviour)* |
| `1` reliable | **producer backpressure**: the write answers `tr::flow::backpressure` (`FLOW_BACKPRESSURE`, the standing nothrow drop-with-receipt contract) and the delivery is enqueued for no one late. The *producer* decides whether to stall, skip, or degrade. *(prose name: the lossless-window behaviour)* |

- **Conflate ignores the bit** — conflation *is* its pressure contract (the newest
  value replaces the queued one; nothing is "lost" that the class promised to keep).
- **There is no retransmit and no NACK in v1.** Retransmit needs a producer
  retention contract an MCU cannot promise; a recovery scheme can be layered later
  as an application convention over the descriptor without wire changes (§8).
- **Every loss counts into delivery accounting.** Transit loss, ring shed, and
  producer-side skips all increment per-subscription counters a client can read.
  A shed with no gap signal and no accounting is **non-conforming** — silence is
  the one behaviour this RFC forbids.
- `realtime-lossy` / `lossless-window` are **prose descriptions only** (kept above
  for continuity with the superseded draft); no wire field or key carries either
  name, and the `stream_qos` selector is dead (§4.1.1).

### 4.5 The gap signal — `tr::flow::address_shift_gap`, generalized

No new error code. `tr::flow::address_shift_gap` (`0x0042`,
`err_t::FLOW_ADDRESS_SHIFT_GAP`) is **generalized** from its address-shift-slicing
origin to one glossary concept: **a detected discontinuity in an ordered flow** —
the receiver-visible signal that elements which should have arrived in order did
not. Its two ruled contexts:

1. a missing **interior slice** of an address-shift group (the original ADR-0011
   meaning, unchanged);
2. a **ring-overflow shed** under §4.4's best-effort contract.

One code means one receiver-side gap-handling path — a consumer of an ordered flow
handles "something is missing here" identically whether the loss happened in
slicing reassembly or at a producer's ring. The per-context classification
(severity/disposition) is documented at the registry entry
(`core/include/libtracer/error.hpp`), never re-derived per call site. The
[CONTEXT.md](../../../CONTEXT.md) glossary entry is widened **in this RFC's PR**
(the entry text rides this change, not a follow-up).

### 4.6 The stream ring — PMR-backed, reconciled with RFC-0022 §8

> **Amended 2026-08-21 by Amendment 2 (§4.6.1).** The ring described below moves from the
> producer's fan-out edge to the **receiving vertex**, and its bound moves from deliveries to
> **bytes**. Read this section through §4.6.1; the reconciliation logic it states is kept.

The ring behind class-3 delivery draws from the **injected PMR/arena** (the
standing hot-path rule; the same substrate direction as the arena and label-table
work) and is bounded by that resource — not by a synthetic constant.

**Reconciliation with RFC-0022 §8 (which stands untouched): both bounds compose;
neither derives from the other.** The owner **declares** depth
(`set_history_depth` — an application retention *intent* no resource can supply,
exactly as §8 answered); the subscription **requests** its window
(`stream_depth`, §4.1.1); the **injected resource bounds what is satisfiable**. A
shortfall — a declared or requested depth the resource cannot fund at the moment it
is needed — **surfaces through §4.4's pressure contract** (gap or backpressure per
the subscription's `reliability`), never as a silent shrink of the declared depth.

The implementation builds on the **existing STREAM-role ring/drain machinery**
rather than greenfield: this RFC makes that machinery demand-driven (installed by a
class-3 subscription) and resource-bounded, not a new subsystem.

#### 4.6.1 Amendment 2 (2026-08-21, ruled) — a producer never queues: the ring belongs to the consumer

**Instrument.** Amendment, not erratum: §4.6 as written put the ring on the producer's fan-out
edge and sized it in *deliveries*; this moves it to the receiver and sizes it in *bytes*, and it
retires the `stream_depth` request key of §4.1.1. No wire byte, no type code, no `opt` bit and no
`delivery_policy` bit moves — the relocation is entirely a property of where an implementation
holds state — but it changes a normative *where*, so it is an amendment. Ruled by the maintainer
in the 2026-08-20 grilling session (Q8), on the measurement banked in §4.6.2. The comment window
is waived by default under [GOVERNANCE.md](../../../.github/GOVERNANCE.md)'s solo-maintainer
clause and is not invoked.

##### The judgement

> **A producer never queues. The queue belongs to whoever consumes it, sized in BYTES by that
> party's own injected memory source. The producer knows the consumer's sampling rate and shapes
> traffic to it.**

Everything below is that sentence unpacked.

1. **Producer writes are always lock-free.** The producer-side ring machinery is *deleted*, not
   made optional. A write is the baseline lock-free store on every class, including class 3 —
   there is no depth-dependent producer path and no producer-side accumulation to pay for. §4.6.2
   measures what the deleted machinery cost: a **fixed +29 ns (+54 %) per write regardless of
   depth**.
2. **The stream-class queue materializes at the RECEIVING vertex.** Depth is a property of the
   party that *wants* depth, expressed where that party owns state: a subscriber that wants a
   queue **makes its own target vertex a STREAM**. This is not a new mechanism — it is the
   existing STREAM role, read from the consumer's side, and it is already the ruled reading of the
   ring in [CONTEXT.md](../../../CONTEXT.md) §Element addressing ("the ring is drain-only; a
   consumer wanting a queue makes **its own** receiving vertex a STREAM").
3. **Every ring is bounded in BYTES by that vertex's own injected `mem::block_source_t`** —
   **per-injection-point, never a shared pool**. The byte bound replaces §4.1.1's
   count-of-deliveries bound because a count is not a RAM budget on a variable-size value.
   "Never shared" is not a preference:
   [ADR-0079](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)'s
   2026-08-20 amendment measured a **shared** source collapsing to **0.01x of its own
   single-thread rate at T = 24** — identical to the widest arm — while the **per-thread**
   composition scaled at 0.46x and ran **−9.2 %** on net-leg latency. A shared ring behind a
   fan-out is that same shape at a different granularity. Composition remains **multiple knobs,
   varied always per target** (folded / per-plane / per-thread — no universal default), each of
   the ~12 injection seams defaulting to `&mem::heap_source()`.
4. **§4.4's pressure contract binds at the RECEIVER ring.** Best-effort =
   **drop-oldest + `FLOW_ADDRESS_SHIFT_GAP` + loss accounting**, exactly as §4.4/§4.5 specify,
   applied when the receiving vertex's ring cannot admit. Reliable = **`FLOW_BACKPRESSURE`
   propagated back to the rate-aware producer, which slows**. Silence remains the one
   non-conforming behaviour.
5. **`set_history_depth` stays a HOST-ONLY intent.** It is the owner's retention *declaration* on
   its own vertex, with **no wire surface** — unchanged by this amendment, and now doubly
   coherent: the vertex declaring depth is the vertex holding the ring. The conformance vector
   `stream/history-depth-host-only` (§7) stays valid precisely because nothing about it is on the
   wire.
6. **Cross-writer total order is delegated to
   [ADR-0019](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0019-per-producer-monotonic-origin-timestamp.md)
   per-producer HLC timestamps.** A receiver ring fed by N producers orders by stamp; it does not
   mint a sequence number, and no producer coordinates with another to write.
7. **The per-edge credit window is PARKED**, named here so it is a decision and not an omission:
   it is the **v2 wire-level escalation** for flow control, to be taken up if and only if the
   receiver-ring contract above proves insufficient. It is not v1 surface.
8. **Batch folding (`delivery_class = 2`, batch) is the PREFERRED stream carrier.** §4.6.2
   measures **32.0x per-sample amortization** (2.54 ns/sample), **~9.3 B/sample retained against
   172 B**, and — with Amendment 1's uniform-stream rule — **timestamps at 0 bytes per sample**. A
   producer that batches to the consumer's declared sampling rate is doing the traffic shaping
   this judgement's last clause names.

##### What §4.6 and §4.1.1 keep, and what they lose

- **Kept:** §4.6's reconciliation shape — the owner *declares* retention intent, the injected
  resource *bounds* what is satisfiable, and a shortfall surfaces through §4.4's pressure
  contract rather than as a silent shrink. RFC-0022 §8 still stands untouched.
- **Kept:** the implementation builds on the existing STREAM-role ring/drain machinery rather than
  greenfield.
- **Lost:** the ring's *location* (producer fan-out edge → receiving vertex), its *unit*
  (deliveries → bytes), and the `stream_depth` **subscription request key of §4.1.1**, which is
  **retired**: a subscriber does not request a window from a producer, it sizes its own. A
  `stream_depth` key appearing on the wire is carried verbatim and **ignored**, under the standing
  absent-⇒-default doctrine.
- **Lost:** any reading of §4.8 clause 3 ("the producer retains a ring *because* a stream-class
  subscriber exists") as producer-side retention. The subscription still carries the demand — what
  it installs is append-preserving *delivery*, and the retention it provokes is the **consumer's**.
  §4.8's four arguments for QoS-on-the-subscription are otherwise unaffected.

#### 4.6.2 Evidence for Amendment 2 (measured)

The basis for the judgement above, banked 2026-08-20:

| measurement | figure |
| --- | ---: |
| STREAM ring premium per write | **+29 ns (+54 %)**, **fixed — independent of depth** |
| …decomposed: probe-alloc | 7.4 ns |
| …decomposed: deque op | 4.4 ns |
| …decomposed: double stripe-mutex / drain | ~17 ns |
| Baseline lock-free write (0 subscribers / 1 subscriber) | **53 ns / 71 ns** |
| 4 writers on one vertex — lock-free | **4.59 M/s** |
| 4 writers on one vertex — STREAM | **1.73 M/s** (**0.14x** of the single-threaded rate) |
| Batch folding — per-sample amortization | **32.0x** (**2.54 ns/sample**) |
| Batch folding — bytes retained per sample | **~9.3 B** vs **172 B** unfolded |
| Batch folding — per-sample timestamp cost (uniform, Amendment 1) | **0 B** |

Two readings decide the amendment:

1. **The premium is fixed, not proportional.** +29 ns is paid on *every* write at *every* depth,
   including depth 1 — so a producer-side ring taxes the whole write path for a service only some
   consumers want. Moving it to the receiver makes the party that wants depth the party that pays
   for it.
2. **The producer-side ring does not survive fan-out.** Four writers on one vertex go from
   4.59 M/s lock-free to 1.73 M/s with the ring — 0.14x of the single-threaded rate — which is the
   same collapse shape ADR-0079's amendment measured for a shared allocation store. Both have one
   cause: one shared, locked structure behind a many-writer path.

Contextual composition figures are ADR-0079's, quoted in clause 3 above; they are that ADR's
banked #941 sweep, not a measurement of this RFC.

### 4.7 Cold start, tiers, and the T2 control plane

**Cold start — attach-forward only.** A stream-class subscriber's first delivery is
the **first fan-out event after its edge installs**. The ring is drain machinery
under pressure, **never history replay**: no ring backfill, no epoch rewind, no
"seamless history" promise. `durability_request` (bit 5) keeps its ordinary
meaning — the latched last value as the join gift — and the stream class governs
only deliveries **after** it. A consumer wanting deeper history reads it as data
(the application's concern), not as a subscription feature.

**Tiers.** One model, an injected segment backend, three operating points:

- **T0 — embedded, in-band (≲ 1 Msps).** Batches ride the existing transports;
  producers fill pool-slab segments from DMA and the batch rope forwards hop-to-hop
  without flattening. WS/TCP serves the ≤ 100s-of-kB/s streams; the full 1 Msps
  envelope needs UDP on the same radio (the 1.5 MB/s arithmetic of §2.3).
- **T1 — host LAN.** The same in-band model over the QUIC/WebTransport datagram
  transports; batch-per-datagram alignment is RECOMMENDED so the loss quantum
  equals the accounting quantum.
- **T2 — extreme (1–100 Gsps): the graph is a control plane only.** The
  descriptor's optional `channel` SETTINGS binds an **out-of-band channel**
  (shared-memory ring, RDMA, GPUDirect via the CUDA memory backend — kinds and
  locators opaque to the graph). The graph's whole role is naming, discovery
  (browse/read the descriptor), authorization, negotiation (ordinary descriptor
  writes), and health (the producer keeps liveness current in the descriptor).
  **Sample bytes never enter the graph plane.** The descriptor carries credentials
  **by reference only** (a token id / key handle resolved out-of-band) — the graph
  never carries secrets, and a node MUST treat a channel locator as untrusted
  input. *(Adopted verbatim from the superseded draft per the 2026-08-12 ruling.)*

**The jitter buffer is a receiver-side library helper, never a router duty.**
Playout timing — clock-offset estimation, de-jitter, epoch reset on staleness — is
consumer policy (a recorder wants none of it). The core router stays
store-and-forward; the library SHOULD ship a reference playout helper generalizing
the downstream integration's EWMA-plus-buffer receiver, living **beside** the
graph, not inside it.

### 4.8 Why QoS rides the subscription, not the op

The 2026-08-06 grill chose QoS-on-subscription over a QoS-tag-on-the-op (#863's
open framing) and left the rationale to be pinned here:

1. **The op has one author; the fan-out has many needs.** A write is one frame
   reaching N subscribers — a dashboard wanting conflation, a logger wanting batch,
   a playout engine wanting stream. A tag on the op forces the *producer* to choose
   one policy for all of them, which is the same incoherence RFC-0022 §2 measured
   when these knobs sat per-vertex: a delivery property has no single value across
   a heterogeneous fan-out. The subscription **is** the delivery relationship — the
   only place one value is coherent (the DDS reader/writer precedent RFC-0022
   already cites).
2. **The hot path stays policy-free.** A per-op tag is bytes and a branch on every
   write, paid even when every subscriber wants the default. Per-subscription
   policy is resolved once at admission and cached on the edge — the write path
   carries no QoS bytes and makes no QoS decision.
3. **It is the demand-driven mechanism itself.** The producer retains a ring
   *because a stream-class subscriber exists* — the subscription is what carries
   the demand. A per-op tag cannot express "retain for me"; it can only describe
   the write it rides.
4. **Compatibility is free.** The `qos_settings` SETTINGS child and its
   absent-⇒-default doctrine already exist; a per-op tag would need new grammar on
   the data plane, which §3 forbids.

## 5. Phasing

Each phase lands and is useful independently; the ordering is ratified.

| Phase | Scope | Unblocks |
| --- | --- | --- |
| **1** | Timestamp writer plumbing (§4.2's three fixes; tracked as [#1109](https://github.com/avatarsd-llc/libtracer/issues/1109)) | RTT/latency measurement over FWD; the batch convention's timing — independent of the rest of this RFC |
| **2** | The batch convention (§4.2) + the separate descriptor LKV convention (§4.3) | Embedders ship clocked output streaming immediately — LKV-conflated delivery and all |
| **3** | `delivery_class` bits + magnitudes (§4.1), the PMR ring (§4.6), the pressure contract (§4.4) and generalized gap signal (§4.5) | The normative core: removes conflation for true signal subscribers |

Phase 2 deliberately works **before** phase 3: under conflating delivery a whole
batch is still one value write, so low-rate batched streams (the ≤ 1 Msps embedder
envelope) function correctly end-to-end; phase 3 changes only what happens under
fan-out pressure.

## 6. Files an accepted RFC edits

- `docs/reference/05-protocol-tlvs.md` — the `delivery_policy` bit table gains
  bits 6–7 `delivery_class` (reserved narrows to 8–15); `qos_settings` gains the
  three §4.1.1 magnitude keys; prose for class semantics and the pressure contract.
- `docs/reference/02-graph-model.md` — the delivery-class ladder and the
  attach-forward cold-start boundary.
- `docs/reference/01-data-format.md` — a short worked example of the batch trailer
  convention (`TF=0` parent base + `TF=1` child offsets) — illustrative only; the
  normative forms already exist there.
- `docs/spec/v1.md` — §3 incorporation changelog line.
- `docs/reference/11-vertex-roles-and-aggregation.md`, `docs/reference/09-memory-substrate.md`,
  `docs/reference/04-communication-flows.md` — the ring's location and unit, transcribed to
  Amendment 2 (§4.6.1): the ring is the **receiving** vertex's, bounded in **bytes** by that
  vertex's **own** injected source.
- `CONTEXT.md` — the widened `tr::flow::address_shift_gap` glossary entry
  (**edited in this PR**, §4.5) plus entries for *delivery class* / *batch
  convention* / *stream descriptor* when the reference pages land.
- `core/include/libtracer/error.hpp` — per-context classification note on
  `FLOW_ADDRESS_SHIFT_GAP` (phase 3).
- Conformance vectors — §7.

## 7. Compatibility and conformance

**No wire break.** No new type codes, no new `opt` bits, no new framing. The
`delivery_policy` extension occupies formerly-reserved bits whose ruled default
(`0`) is today's behaviour, and reserved bits were always carried verbatim — old
subscribers are conflate-class by construction, old producers never see a class
they must honour. The magnitude keys are absent-⇒-default NAME-tagged SETTINGS
children, invisible to parsers that predate them. v1 is unreleased, so this is a
draft refinement.

New vectors (the `stream/*` family, phase-3 unless noted):

1. `subscriber/policy-class-absent` — bits 6–7 absent/zero ⇒ conflating delivery,
   byte-identical to today (tightens `subscriber/policy-absent`).
2. `stream/class-immediate-order` — class 1 delivers every write, in order, none
   conflated.
3. `stream/class-batch-flush` — class 2 flushes on `batch_count` and on
   `batch_window_ns`, whichever first.
4. `stream/class-stream-no-conflate` — class 3 never conflates under a lagging
   consumer.
5. `stream/besteffort-shed-gap` — best-effort overflow sheds oldest whole and
   surfaces `tr::flow::address_shift_gap` in-order, with accounting incremented.
6. `stream/reliable-backpressure` — reliable overflow answers
   `tr::flow::backpressure` at the producer; nothing is delivered late.
7. `stream/attach-forward` — a class-3 subscriber's first delivery is the first
   post-attach fan-out; with `durability_request` set, the latch precedes it.
8. ~~`stream/batch-trailer-roundtrip`~~ → **`stream/batch-time-roundtrip`** (phase 1/2,
   restated by Amendment 1 §4.2.1) — a batch carrying a payload `TIME{u64 base}` child
   (plus, for a non-uniform stream, its packed `i32` offset array) survives origin → FWD
   hop → delivery intact, and the **outermost** frame's trailer TS is `TF=0` wire/TX time
   at every hop.
9. `stream/loss-accounting` — every shed/skip increments a counter a client can
   read.
10. `stream/history-depth-host-only` (Amendment 2 §4.6.1) — `set_history_depth` has **no
    wire surface**: a peer write of any `history_keep_last`-shaped `:settings` knob answers
    `SCHEMA_NOT_FOUND`, and no read exposes the declared depth. Unaffected by the ring's
    relocation, precisely because the relocation is host-side only.
11. `stream/tf1-reserved` (Amendment 1 §4.2.1, Q10) — a `TF=1` frame decodes and records
    its flag and delta rather than being rejected at decode; a relay carries it verbatim;
    the reply-echo path declines a `TF=1` root.
12. `stream/receiver-ring-flood` (Amendment 2 §4.6.1) — exhausting the **net-plane** store
    under a flood leaves the **graph plane** still able to allocate; the receiver ring
    sheds under §4.4 rather than starving the node.

**Migration.** Existing app-level conventions keep working untouched — they are
opaque `VALUE` writes and remain so. The downstream scope and PWM-timeline vertices
migrate by moving their bespoke headers into the descriptor + batch-trailer
convention 1:1 and shrinking their receiver code to the library playout helper; no
dual-publish transition is needed because nothing about the data vertex changes.

## 8. Alternatives considered

1. **A new `0x20 STREAM_BATCH` TLV with a fixed `{epoch, seq, t0, count}` header**
   (the superseded draft's §B). Rejected — 2026-08-12 ruling 1: zero new grammar
   wins. The batch is fully expressible and fully decodable in the existing format
   today; a new type code buys a hot-path header at the price of a registry
   assignment, a second time-carrying mechanism beside the trailer, and a
   conformance surface for a shape the format already has. The `(epoch, seq)`
   ordering key dies with the header; ordered-flow discontinuity is surfaced by the
   §4.5 gap signal instead, and a recovery scheme wanting explicit naming can layer
   a sequence convention over the descriptor later, without wire changes.
2. **A per-vertex `stream = 1` storage policy** (draft §A). Rejected — ruling 1: the
   plane is demand-driven and init-free. A vertex-init flag re-introduces per-vertex
   delivery policy one RFC after RFC-0022 deleted it, breaks claim 5, and makes
   capability something a vertex must declare rather than something a subscription
   demands.
3. **Descriptor stored as the stream vertex's value, batches deliver-only**
   (draft §A/§C). Rejected — ruling 1: the descriptor is a separate LKV. Storing it
   in the data vertex forks `read`'s meaning per vertex kind and needs exactly the
   per-vertex flag alternative 2 rejects; a sibling LKV gets browse, latch, ACL and
   dashboards for free with zero special cases.
4. **`stream_qos` / `stream_window` as `qos_settings` keys** (draft §D). Rejected —
   ruling 3: the pressure contract rides the surviving `reliability` bits × the
   class; a parallel selector would give one subscription two ways to say
   "reliable". The magnitudes go full-width, keyed by class (§4.1.1).
5. **QoS-tag-on-the-op** (#863's alternative framing). Rejected — §4.8.
6. **Per-sample values, no batching.** A TLV header + routing walk per sample is
   ≥ an order of magnitude of overhead at 1 Msps and dimensionally impossible at
   T1+. Rejected without ceremony.
7. **Delegate signals to a sidecar protocol (RTP/WebRTC/SRT).** The streams then
   live outside the graph's naming, discovery, ACL and health model, and there is
   no embedded story. Rejected as the general answer; §4.7's T2 binding
   deliberately leaves room to bind such a session as an out-of-band data plane
   where it fits.
8. **Retransmit / NACK in v1.** Deferred — it needs a producer retention contract
   an MCU cannot promise, and it can be layered later as an application convention
   over the descriptor. Losses-must-count stands regardless.
9. **Rate caps / dirty tracking / scheduling in the library.** Out — the producer
   owns cadence (standing Branch-write rule); a batching timer is application or
   fan-out-edge mechanics under §4.1's declared magnitudes, never a graph-wide
   throttle.

## 9. Relationship to the superseded draft (PR #893)

This document is the rewrite of PR #893's in-comment draft under the 2026-08-12
rulings. The draft's direction — signal plane over existing machinery, per-
subscription QoS, loss-is-observable, tiered data planes, control-plane-only T2 —
is confirmed. Its three forks resolved **against** the draft's side: no
`STREAM_BATCH` type (§8.1), no per-vertex stream flag (§8.2), descriptor as a
separate LKV (§8.3). Its `stream_qos`/`stream_window` keys are dropped (§8.4).
Kept verbatim: credentials-by-reference, the receiver-side jitter-buffer helper,
retransmit deferred, and losses-must-count.

## Discussion

The seven residue decisions of the 2026-08-06 grill were settled in the dedicated
2026-08-12 grilling session recorded on
[#879](https://github.com/avatarsd-llc/libtracer/issues/879); this document is
their transcription and the residue is empty. Deferred items — explicitly future
work, not open questions of this RFC: a retransmit/NACK layer (§8.8), descriptor
format-id curation beyond the application-defined space (§4.3), T2 negotiation
handshake details beyond the descriptor-write mechanism (§4.7), and the scope of
the reference playout helper. Sustained objections and their resolution will be
recorded here; the comment window is waived by default per GOVERNANCE while
solo-maintained.
