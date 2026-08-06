<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0025 — Stream-class values: the high-bandwidth signal plane

| Field | Value |
| ---- | ---- |
| **RFC** | 0025 |
| **Title** | Stream-class values: the high-bandwidth signal plane |
| **Status** | **in-comment** (opened 2026-08-06) |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-08-06 |
| **Comment window closes** | 2026-08-20 (≥ 14 days; waivable per GOVERNANCE.md single-maintainer rule) |
| **Tracking issue** | [#879](https://github.com/avatarsd-llc/libtracer/issues/879) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

A vertex today carries a **last-known value**: writes replace, delivery may conflate,
and a read answers "what is it now?". A *signal* — audio, video, an ADC capture —
answers a different question ("what happened, in order?") and is structurally
mis-served by LKV semantics: samples are not idempotent, order and completeness are
part of the meaning, and conflation is data loss with no receipt. This RFC adds a
**stream class** to the value plane: one new core type code, **`0x20` STREAM_BATCH**
(the first assignment in the long-term registry), carrying a byte-precise
`{epoch, seq, t0, count}` header plus an opaque sample region; a **per-vertex
`stream` storage policy** under which batch writes are *delivered, never stored* —
the vertex's stored value remains a browseable **stream descriptor**; and two
**per-subscription QoS keys** selecting `realtime-lossy` (bounded latency,
drop-oldest) or `lossless-window` (bounded window, producer sees `BACKPRESSURE`)
semantics, composing with RFC-0022's `delivery_policy`. Loss is always an
**observable gap** in `(epoch, seq)`, never silence. The design is tiered: batches
ride the existing delivery machinery in-band up to LAN rates; at extreme rates
(≳ 1 GS/s hosts) the graph carries **only** the descriptor, negotiation, authz and
health of an **out-of-band channel** — the graph never carries those samples.

## Motivation

1. **LKV + conflation is the wrong contract for signals.** Delivery selection is
   deliberately structural and value-agnostic (RFC-0008), storage is
   replace-in-place, and RFC-0022's policy machinery tunes *which last value* a
   subscriber sees — every axis optimises "current state", none expresses "complete
   ordered history at rate". A signal consumer needs ordering, gap visibility, and
   a latency/completeness trade the *subscriber* chooses.

2. **Downstream has already invented this twice, divergently.** A downstream
   firmware (ESP32-C6) ships two hand-rolled stream conventions over opaque
   `VALUE` payloads: a 2 kHz ADC scope emitting `{u32 t0_us, u16 dt_us, u16 n}`
   sample batches, and a PWM timeline (its ADR-0009) with
   `{ver, nch, dt_us, t0_us, nframes}` batches plus receiver-side clock-offset
   EWMA, a jitter buffer, and epoch reset on staleness. Both re-solved framing,
   timing, and loss handling; neither gets protocol-level loss accounting,
   conformance coverage, or reuse. Duplication downstream is the classic signal
   that a primitive is missing one layer up.

3. **The rate envelope spans six orders of magnitude.** The maintainer's target:
   ~1 MS/s on embedded nodes; 1–100 GS/s between high-performance hosts.
   1 MS/s × 12-bit packed ≈ **1.5 MB/s** — already at or above an ESP32-C6
   WebSocket link's practical ceiling, so even the embedded tier needs datagram
   transports and zero-copy discipline. 100 GS/s is ≥ 100 GB/s: no graph router
   should ever route that. One model must serve both ends *without* pretending
   they share a data plane — hence the explicit non-goal below.

**Non-goal.** At the extreme tier the graph is a **control plane only**: it names
the stream, negotiates the channel, authorises subscribers and observes health.
The samples travel out-of-band (shared-memory ring, RDMA/DPDK, GPUDirect via the
CUDA memory backend of ADR-0024). This RFC specifies the *binding*, not those
transports.

## Proposed change

### §A — The stream vertex: descriptor-as-value, deliver-only batches

A vertex becomes **stream-class** by carrying the per-vertex setting
`stream = 1` in its `:settings` (a new named key, same mechanism as every
per-vertex policy after RFC-0022 — storage policy remains per-vertex).

Normative semantics for a stream-class vertex:

- Its **stored value is the stream descriptor** (§C). `READ` returns the
  descriptor; browse, `:schema`, dumps and durability latches see *only* the
  descriptor. The RFC-0022 durability latch therefore replays the descriptor to a
  late joiner, never a stale batch.
- A write of a **STREAM_BATCH** TLV (§B) to a stream-class vertex MUST be
  **fanned out to subscribers and MUST NOT replace the stored value**
  (deliver-only). Delivery uses the existing machinery unchanged — the written
  TLV as-is, vertical bubbling per RFC-0005, remote delivery over the return
  route — with one carve-out: **conflation and every "newest wins" optimisation
  are structurally inapplicable to STREAM_BATCH frames**; an implementation that
  cannot enqueue a batch for a subscriber drops it *whole* and accounts it (§D).
- A write of a **non-batch** TLV to a stream-class vertex updates the descriptor
  normally (it is just a value write); producers SHOULD write the descriptor on
  epoch change (§C).
- A STREAM_BATCH written to a **non-stream vertex** is invalid:
  `ERROR{tr::value::type_mismatch}`. This keeps the "batches are never LKV"
  invariant decidable at the write site.

### §B — Wire shape: `0x20` STREAM_BATCH

One new core type code, the first assignment in the long-term registry
(`0x20`–`0x7F`). Not structured (`opt.PL = 0`): the payload is a fixed
little-endian header followed by an opaque sample region.

```
type = 0x20  STREAM_BATCH        opt.PL = 0
payload:
  u8   ver          ; = 1
  u8   flags        ; bit0 SOURCE_GAP  — producer knowingly skipped samples
                    ; bit1 EPOCH_START — first batch of this epoch
                    ; bits 2–7 reserved: MUST be written 0, MUST be ignored
  u16  format       ; sample format id — mirrors the descriptor (§C);
                    ;   receivers MUST drop (and account) a batch whose format
                    ;   disagrees with the current descriptor epoch's
  u32  epoch        ; capture epoch; a new epoch resets seq and re-bases t0
  u32  seq          ; per-epoch batch sequence, first batch = 0
  u64  t0_ns        ; producer-clock time of the first sample frame, ns
  u32  count        ; number of sample FRAMES in this batch (frame = one
                    ;   sample per channel, channel count from the descriptor)
  [ samples ]       ; count frames, packed per the descriptor's format —
                    ;   opaque to the router, contiguous, zero-copy sliceable
```

Header is 24 bytes. Design notes (normative where stated):

- **Fixed header, not TLV children.** A TIME + VALUE composition (the
  reference/05 `0x0C` pattern) costs per-field TLV headers and a parse walk on
  every batch; at 1 kHz batch rate on an MCU the fixed layout is decodable with
  one bounds check and feeds DMA-filled pool slabs without re-encoding
  (ADR-0041/0042 one-copy discipline; multi-segment batches forward hop-to-hop
  without flattening per the ADR-0053 rope path). The wire-trailer `opt.TS`
  remains available and remains *transport* time — `t0_ns` is application-domain
  time, per the reference/05 TIME distinction.
- **Ordering key is `(epoch, seq)`.** `seq` MUST increment by exactly 1 within
  an epoch. A receiver observing `seq` advance by more than 1 has detected
  **transit loss**; `flags.SOURCE_GAP` distinguishes source-side skips (producer
  overrun) from transit loss. Both are observable events (§D) — silence is
  non-conforming.
- **Batch size** is bounded by the standard `u16` length (LL widening permitted
  but NOT RECOMMENDED for streams); embedded producers SHOULD size batches to
  their transport datagram/frame budget (§F). `count = 0` is valid (a heartbeat
  carrying epoch/seq liveness through a silent source).
- Unknown-type safety: `0x20` sits in the registry range old receivers already
  skip safely per reference/01 §handling unknown type codes — a pre-RFC node
  forwards or ignores batches without harm (and a pre-RFC *router* still
  delivers them as opaque writes; it merely lacks the no-conflate carve-out,
  which is why §E's capability gate exists).

### §C — The descriptor

The stream-class vertex's stored value is a `SETTINGS` TLV (`0x0B` — named
fields, already browseable by every existing tool):

```
SETTINGS {
  NAME "format"    VALUE u16      ; sample format id (registry below)
  NAME "channels"  VALUE u8       ; frames are channel-interleaved
  NAME "dt_ns"     VALUE u64      ; nominal sample period; 0 = irregular
                                  ;   (then per-batch t0 carries all timing)
  NAME "epoch"     VALUE u32      ; current epoch (mirrors batches)
  NAME "qos"       VALUE u8       ; producer default: 0 realtime-lossy,
                                  ;   1 lossless-window (subscriber may override, §D)
  NAME "channel"   SETTINGS {…}   ; optional — T2 out-of-band binding (§E)
}
```

Format ids `0x0000`–`0x7FFF` are a spec registry seeded with the obvious PCM
shapes (`u8`, `u12` packed, `u16`, `i16`, `f32`); `0x8000`–`0xFFFF` are
application-defined (video codecs, compressed audio — the graph does not
transcode). The descriptor is a normal value: subscribing to a stream vertex
delivers the descriptor via the durability latch first (join), then batches.

### §D — Subscription QoS and flow control

Two new `qos_settings` keys on the SUBSCRIBER record (same extension mechanism
as `delivery_policy` — absent ⇒ default, byte-identical to today):

```
NAME "stream_qos"     VALUE u8    ; 0 = realtime-lossy (default)
                                  ; 1 = lossless-window
NAME "stream_window"  VALUE u16   ; per-subscription queue depth, in BATCHES;
                                  ;   0 ⇒ implementation default (MUST be ≥ 2)
```

- **`realtime-lossy`** — bounded latency. The per-subscription queue holds at
  most `stream_window` batches; on overflow the implementation MUST drop the
  **oldest** queued batch (whole-batch, never partial) and increment that
  subscription's gap accounting. The consumer's receive path observes the seq
  gap exactly as it would transit loss. Latency is bounded by
  `stream_window × batch duration`; completeness is sacrificed knowingly.
- **`lossless-window`** — bounded completeness. On overflow the write at the
  producer returns the existing **`BACKPRESSURE`** status (the same nothrow
  drop-with-receipt contract the substrate uses everywhere) and the batch is
  delivered to no one late — the *producer* decides whether to stall, skip
  (setting `SOURCE_GAP`), or degrade. There is **no retransmit and no NACK in
  this RFC** (deferred, §Open questions): retransmit requires producer-side
  batch retention that an MCU cannot promise and a latency class that
  contradicts `realtime-lossy`; a NACK scheme can be layered later without wire
  changes (the `(epoch, seq)` key already names every batch).
- Composition with RFC-0022 `delivery_policy`: `durability_request` applies to
  the **descriptor only** (§A); `reliability = 1` — stored but unimplemented
  today — is *subsumed* for streams by `stream_qos = 1` and remains the
  transport-level knob for non-stream values. `priority` applies unchanged.
- **Accounting.** Per-subscription counters — batches delivered, batches
  dropped (queue overflow), gaps observed — join the existing delivery-drop
  accounting surface. Every loss path increments a counter a client can read;
  this is the "loss is observable, silence is non-conforming" invariant in
  mechanism form.
- **The jitter buffer is a receiver-side library service, not a router duty.**
  Playout timing (offset estimation, de-jitter, epoch-reset) is consumer policy
  — the downstream EWMA-plus-buffer is one policy among many (a recorder wants
  none). The core router stays store-and-forward. Because every consumer of a
  periodic stream will need one, the *library* SHOULD ship a reference playout
  helper (the ADR-0009 semantics, generalised) — but it lives beside the graph,
  not inside it.

### §E — Tiered data planes

- **T0 — embedded, in-band (≲ 1 MS/s).** Batches ride the existing transports.
  Arithmetic that sizes the tier: 1 MS/s × 12-bit packed = 1.5 MB/s — above a
  C6-class WebSocket/TCP link's practical budget, within UDP datagram reach on
  the same radio. Producers fill pool-slab payloads directly from DMA (the
  scope pattern), the batch TLV is emitted around the slab (one copy at most),
  and forwarding hops move the rope without flattening. WS/TCP remains fine for
  the ≤ 100s-of-kB/s streams (audio-rate telemetry, that ADC scope).
- **T1 — host LAN (≲ ~1 GS/s equivalent bandwidth).** Same in-band model over
  the QUIC / WebTransport datagram transports; kernel sockets with offloads
  carry it. Batch-per-datagram alignment is RECOMMENDED (loss quantum = the
  accounting quantum).
- **T2 — extreme (1–100 GS/s).** The descriptor's `channel` SETTINGS binds an
  **out-of-band channel**:

  ```
  NAME "channel" SETTINGS {
    NAME "kind"     VALUE u8      ; 0 shm-ring, 1 rdma, 2 gpudirect, 3 udp-mcast,
                                  ;   ≥ 0x80 application-defined
    NAME "locator"  VALUE bytes   ; kind-specific, opaque to the graph
                                  ;   (ring name+size / RDMA QP descriptor / group+port)
    NAME "cred"     VALUE bytes   ; capability REFERENCE (§F) — never the secret
    NAME "state"    VALUE u8      ; 0 down, 1 negotiating, 2 up — producer-owned
  }
  ```

  The graph's role, in full: **naming** (the stream is a vertex like any
  other), **discovery** (browse/READ the descriptor), **authz** (§F),
  **negotiation** (a subscriber writes its capability/receiver locator to an
  agreed request field; the producer answers by updating `channel`), and
  **health** (the producer MUST keep `state`, `epoch` and a coarse liveness
  counter current in the descriptor — that update *is* an ordinary low-rate
  value write, so existing subscribers/dashboards observe T2 stream health with
  zero new machinery). Samples never enter the graph plane. GPU-resident sinks
  compose with the ADR-0024 CUDA memory backend (a batch landing in device
  memory is that backend's rope segment); intra-host uses a shm ring; inter-host
  RDMA/DPDK — all deliberately outside this spec beyond the binding above.

### §F — Security and resource bounds

- **Authz.** A stream subscribe is a subscribe: the existing per-vertex ACL
  gates it (READ right for the descriptor, SUBSCRIBE for delivery). For T2, the
  descriptor's `cred` field carries a **reference** (token id / key handle)
  resolved out-of-band; secrets MUST NOT transit the graph in descriptor
  plaintext. A node MUST treat `channel.locator` as untrusted input.
- **Bounds** (RFC-0006 spirit — every resource explicitly bounded):
  - Batch payload ≤ `u16` length budget; embedded profile RECOMMENDS ≤ 4 KiB
    per batch (fits one slab class + one radio frame comfortably).
  - Per-subscription queue memory = `stream_window × max batch size`, bounded
    by admission: an implementation MAY refuse a subscribe whose window it
    cannot fund (`ERROR{resource}` today), never silently shrink it.
  - Streams per node and stream subscribers per vertex are implementation
    limits and MUST be published the way other node limits are (the identity
    facet of RFC-0011).
  - The C6-class worked example: a 2 kHz × f32 scope batch at 50 Hz batching is
    ~330 B/batch — in-band WS with room to spare; the full 1 MS/s ADC needs
    12-bit packing, 4 KiB batches (~2.7 kHz batch rate), UDP, and one dedicated
    slab pool — feasible, tight, and *visible* in the accounting when it isn't.

### §G — Migration

Existing app-level conventions keep working untouched — they are opaque VALUE
writes and remain so. Downstream migration maps those two conventions
onto the primitive: the scope vertex and the PWM-timeline vertex become
stream-class (descriptor + `0x20` batches; the timeline's `{nch, dt_us, t0_us}`
header fields move into descriptor + batch header 1:1), and their bespoke
receiver code shrinks to the library playout helper. A transition can
dual-publish (old VALUE batches on the old vertex, STREAM_BATCH on a new one)
since the classes cannot collide. The `stream` setting is refused by old nodes'
`:settings` validation as an unknown key — which is the correct failure: a
stream vertex on a pre-RFC node would silently store batches as LKV, so
**capability MUST be explicit**, not sniffed.

### §H — Files an accepted RFC edits

- `docs/reference/05-protocol-tlvs.md` — new §`0x20` STREAM_BATCH; SUBSCRIBER
  `qos_settings` gains the two stream keys; registry note that `0x20` opens the
  long-term range.
- `docs/reference/02-graph-model.md` — stream-class vertex semantics (§A).
- `docs/spec/v1.md` — §3 incorporation changelog line.
- Conformance vectors (new `stream/*` family): batch round-trip; header
  byte-pin; seq-gap detection; drop-oldest overflow; `BACKPRESSURE` overflow;
  descriptor-then-batches join order; batch-to-non-stream-vertex rejection;
  unknown-type skip regression for `0x20` against a pre-RFC decoder.
- `CONTEXT.md` — glossary entries: *stream*, *batch*, *epoch*, *channel (T2)*.

## Compatibility

- **No protocol break.** `0x20` was unassigned (skip-safe for old receivers);
  new SETTINGS keys are absent-⇒-default; the `stream` per-vertex setting is
  opt-in and MUST be explicitly supported (§G). v1 is DRAFT, so this is a draft
  refinement, not a v2.
- **New conformance vectors** as listed in §H; no existing vector changes
  meaning.
- **Deployed devices** (the downstream fleet): unaffected until a firmware adopts
  stream-class vertices; the dual-publish path in §G covers mixed fleets.

## Alternatives considered

1. **Status quo — per-app batch conventions over VALUE.** Works (it shipped,
   twice), but every consumer reinvents framing/timing/loss, gets zero
   protocol-level loss accounting, and remains exposed to the conflation
   machinery's assumption that newest-wins is harmless. The duplication is the
   evidence; rejected.
2. **Delegate signals wholly to a sidecar protocol (RTP/WebRTC/SRT).** Mature
   data planes, but the streams then live outside the graph's naming,
   discovery, ACL and health model — exactly the split §E's T2 design avoids by
   keeping the *control* plane in-graph. Also no embedded story on a C6-class
   node. Rejected as the general answer; note that §E's T2 channel `kind ≥ 0x80`
   deliberately leaves room to bind an RTP session as the data plane where it
   fits.
3. **Per-sample values, no batching.** Each sample as a VALUE write costs a TLV
   header + routing walk per sample: ≥ an order of magnitude of overhead at
   1 MS/s and gigabytes of header traffic at T1 — dimensionally impossible.
   Rejected without ceremony.
4. **A structured (`PL=1`) batch of TIME + VALUE children.** Grammar-pure but
   pays per-field TLV overhead and a parse walk per batch on the hot path, and
   invites partial-batch slicing that breaks the whole-batch loss quantum.
   Rejected for the fixed header (§B) — precedent: FWD and the route-handle
   frames already use purpose-built layouts where the hot path demands it.
5. **gRPC-style streaming bolt-on beside the graph.** Foreign framing and TLS
   stack, no MCU story, and a second addressing scheme for the same resources.
   Rejected.
6. **Conflation-exempt flag on ordinary VALUE writes** (no new type). Smallest
   diff, but it overloads VALUE's meaning per-vertex-state, makes the batch
   header an unspecified app convention again, and gives conformance nothing to
   pin. The type code is what makes the class decidable everywhere. Rejected.

## Discussion

Open questions deliberately deferred (tracked on
[#879](https://github.com/avatarsd-llc/libtracer/issues/879)):

1. **NACK/retransmit for `lossless-window`** — layerable later over the
   `(epoch, seq)` naming; needs a producer retention contract first.
2. **Format registry curation** — which PCM/video ids ship in the first table,
   and whether compressed formats carry parameter blobs in the descriptor.
3. **T2 negotiation details** — the request-field handshake shape (subscriber →
   producer) is sketched, not pinned; RDMA/GPUDirect credential lifecycles need
   implementer input.
4. **Subtree interaction** — a stream vertex with children (RFC-0005 bubbling of
   batches to ancestor subscribers) is permitted by construction; whether
   ancestors *want* batch traffic or should see descriptor-only is unresolved.
5. **Reference playout helper scope** — how much of the ADR-0009 receiver
   (offset EWMA, jitter buffer, epoch reset) the library ships versus documents.

Per GOVERNANCE.md the tracking issue stays open ≥ 14 days for implementer
feedback before this document is ratified. Sustained objections and their
resolution will be recorded here.
