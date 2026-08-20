# Reference 01 — Data format

> **Status**: normative, v1, 2026-05-03 (incorporated by [docs/spec/v1.md](../spec/v1.md) §3 per RFC-0001 §A.2). Byte-precise definition of every libtracer frame on the wire. A second-implementer SHOULD be able to write an interoperable parser/sender from this section alone.
> **See also**: design rationale (CRC choice, atomic ordering, MCU stack safety) is in the [ADR set](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/) and git history. For a worked byte-level walkthrough of these same rules — annotated frames, one byte at a time — see [wire format, bit by bit](../modules/wire-format-bits.md). To inspect real libtracer frames on the wire, the [**Wireshark dissector**](https://github.com/avatarsd-llc/libtracer/tree/main/tools/wireshark) decodes this format live (a single-file Lua dissector, vector-tested against the conformance frames).

---

## Frame layout

Every libtracer TLV is a **header + payload + optional trailer**. The payload is a contiguous, untouched user region; metadata never interleaves with it.

```
Offset      Field         Width       Notes
----------  ------------  ----------  -------------------------------------------
0           type          u8          TLV type code (see §Type code registry)
1           opt           u8          Bit-packed options (see §Options bitfield)
2           length        u16 LE      Payload byte count if opt.LL = 0 (default)
                          u32 LE      if opt.LL = 1
H           payload       length × u8 Pure user region — no metadata interleave
                                      H = 4 if opt.LL = 0, else 6
H + L       trailer_ts    u64 LE      Optional, present iff opt.TS = 1, opt.TF = 0
                                      Absolute wire-time, ns since Unix epoch
                          i32 LE      Optional, present iff opt.TS = 1, opt.TF = 1
                                      Relative offset from parent TS, ns
H + L + T   trailer_crc   u32 LE      Optional, present iff opt.CR = 1, opt.CW = 0
                                      CRC-32C
                          u16 LE      Optional, present iff opt.CR = 1, opt.CW = 1
                                      CRC-16-CCITT
```

Where `L = length`, `T = (8 if TS=1 and TF=0) | (4 if TS=1 and TF=1) | 0 if TS=0`.

Total frame size: `H + L + T + (4 if CR=1 and CW=0) + (2 if CR=1 and CW=1)`.

- **Endianness**: little-endian for every multi-byte field. Matches Cortex-M, ARMv8, x86, ESP32; no per-platform byte swap.
- **Alignment**: header is **packed**, not naturally aligned. Implementations MUST tolerate unaligned reads of `length`, `trailer_ts`, `trailer_crc`. (The default `LL=0` case keeps payload at offset 4, naturally aligned for u32 access.)
- **Minimum frame**: 4 bytes (empty payload, no trailer, default `LL=0`) — the empty STATUS=OK signal.

### Append-only-at-egress, strip-only-at-ingress

The trailer's purpose is to keep the **payload region byte-identical across boundaries**. A TLV exists in three states without the payload bytes ever moving:

```
At rest (in graph storage, in a recorder file):
  [ header ] [ payload ]                       ← H + L bytes

In transit (on a transport, with optional integrity + wire time):
  [ header ] [ payload ] [ trailer_ts? ] [ trailer_crc? ]
                                               ← H + L + 0..12 bytes

At rest again (after received, validated, stripped):
  [ header ] [ payload ]                       ← H + L bytes (same payload bytes)
```

A forwarder re-emitting on another transport: strip incoming trailer, attach outgoing trailer (fresh wire-time, fresh CRC). A recorder writing to disk: strip the trailer, store header+payload. On replay: re-attach a fresh trailer. **Payload bytes are invariant under every transition.**

This symmetry is what makes the same-substrate insight extend cleanly across multi-hop forwarding and recording. See [02-graph-model.md](02-graph-model.md) §the trailer enables payload-bytes invariance.

---

## Options bitfield

```
bit 7  6  5  4  3  2  1  0
    +--+--+--+--+--+--+--+--+
    |R |PL|TS|CR|LL|CW|TF|R |
    +--+--+--+--+--+--+--+--+

R   Reserved (bit 7). MUST be zero. Receivers MUST reject non-zero as INVALID.
PL  Payload-is-structured. 0 = opaque bytes.   1 = concatenated child TLVs.
TS  Trailer has timestamp. See TF for form.
CR  Trailer has CRC.       See CW for width.
LL  Length width.          0 = u16 (2 bytes, ≤ 64 KiB payload). DEFAULT.
                           1 = u32 (4 bytes, ≤ 4 GiB payload).
CW  CRC width.             0 = CRC-32C (4 bytes). DEFAULT.
                           1 = CRC-16-CCITT (2 bytes).
                           Meaningful only when CR = 1.
TF  Timestamp form.        0 = absolute u64 ns since Unix epoch (8 bytes). DEFAULT.
                           1 = signed i32 ns offset from parent TS (4 bytes).
                           Meaningful only when TS = 1.
R   Reserved (bit 0). MUST be zero. Receivers MUST reject non-zero as INVALID.
```

Two reserved bits remain (bits 7 and 0) for unforeseen L2 needs.

**Reserved bits are committed for the lifetime of v1.** They are not "reserved for later minor evolution"; protocol v1 is immutable, so they are forever-frozen. A protocol-v1 receiver that observes a reserved bit set MUST reject the TLV as `INVALID`, and the spec MUST NOT allocate them within v1. Forward-compatible extensions live exclusively in the unassigned reaches of the type-code registry ([05-protocol-tlvs.md](05-protocol-tlvs.md) is the registry of record); incompatible changes live at the discovery layer per §versioning. This makes it safe for receivers to harden the reserved-bit check at compile time without anticipating future thaw.

### Default vs extended forms

The default header is **4 bytes**. A typical small TLV has no trailer (4-byte total) or a small trailer (CRC-16 only, 6 bytes total). The extended forms (`LL=1`, full TS, full CRC) are opt-in and pay only for what they buy.

### Why no priority bits

There are no priority bits in `opt`. Priority is a **transport-time, per-link, non-coherent** concern; the L2 header carries only coherent things or things every forwarder must see. Per-TLV priority bits would buy nothing that the subscription's cached priority at the egress link doesn't already cover ([RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.A bits 2-4; a per-vertex knob until then, because one vertex fanning out to a CAN peer and a WebSocket peer has no single priority to hold). See [02-graph-model.md](02-graph-model.md) §the six-layer model — priority lives at L4.

---

## Length encoding

Fixed-width unsigned little-endian. Two widths, selected per-TLV by `opt.LL`:

| `opt.LL` | Field width | Max payload | Use when |
| ---- | ---- | ---- | ---- |
| 0 (default) | u16 (2 bytes) | 65535 bytes | Most TLVs |
| 1 | u32 (4 bytes) | 4 GiB − 1 | A single large TLV that for some reason cannot be address-shifted |

### Why no u64 length

Intentional. Capping at u32 means a minimum-feature implementation knows its worst-case segment-pool max in advance and never overflows. A feature-rich host that wants to ship multi-gigabyte single TLVs **MUST** use address-shift slicing across `ep[0..N]` (see [03-addressing.md](03-addressing.md) §address-shift slicing) — exactly the design discipline already imposed by the no-fragmentation principle.

This is a deliberate interop ceiling: minimum impls can communicate with feature-rich hosts without being overwhelmed by single multi-GB frames.

### Why fixed-width, not LEB128

- **Branchless parse** at offset 2: read 2 or 4 bytes based on one bit.
- **Streaming-friendly**: receiver knows the full payload extent immediately and can DMA / mmap the entire payload region without byte-by-byte decoding.
- **Predictable for SIMD and cycle-bound MCU loops.**

(LEB128 and a "finite-pool" mode are rejected designs — see §rejected designs at end.)

---

## CRC

When `opt.CR = 1`, a checksum is appended to the trailer. Width selected by `opt.CW`:

### CRC-32C (default, `opt.CW = 0`)

Castagnoli polynomial `0x1EDC6F41`, reflected representation `0x82F63B78`.

- **Initial value**: `0xFFFFFFFF`.
- **Final XOR**: `0xFFFFFFFF`.
- **Field width on the wire**: 32 bits, little-endian.
- **Hardware acceleration**: x86 SSE 4.2 (`_mm_crc32_*`), ARMv8 `+crc` (`__crc32c*`). Cortex-M software implementation runs ~2 cycles/byte with a 256-entry table.

### CRC-16-CCITT (`opt.CW = 1`)

Polynomial `0x1021`, common variant.

- **Initial value**: `0xFFFF`.
- **Final XOR**: `0x0000`.
- **Field width on the wire**: 16 bits, little-endian.
- **Why offer it**: 2-byte savings per TLV with CRC. False-positive rate ~`1/65536` versus ~`1/2^32` for CRC-32C — acceptable for short messages on tightly-bounded buses (CAN, UART) where the L2 medium itself adds another integrity check.

Both variants are mandatory for conforming receivers. A receiver that sees `CR=1` MUST verify whichever variant `CW` selects; mismatches return `ERROR{tr::frame::crc_fail}`.

### Coverage

CRC is computed over **payload bytes** plus, if present, **`trailer_ts` bytes** (in that order). The header (`type`, `opt`, `length`) is NOT included.

- Streaming send: stream payload while accumulating CRC. At end, generate `trailer_ts` (folded into accumulator), append, then append final CRC.
- Streaming receive: stream payload bytes into segment while accumulating CRC. Read `trailer_ts` (folded). Read CRC, compare to accumulator.

A corrupted timestamp shows up as `CRC_FAIL`, not silent bogus time. The 8 (or 4) trailer-TS bytes folded into the accumulator after the payload loop is trivially cheap.

---

## Timestamp form

When `opt.TS = 1`, a wire-time stamp is appended before the CRC. Form selected by `opt.TF`:

### Absolute (default, `opt.TF = 0`)

```
[ u64 ns_since_unix_epoch_le ]   ; 8 bytes
```

Self-contained: the receiver gets a globally-comparable wire-time without context. Use for top-level TLVs, infrequent writes, anything not part of a tight stream.

Wraparound: year 2554 (584 years from 1970). Acceptable.

### Relative (`opt.TF = 1`)

```
[ i32 ns_offset_from_parent_ts_le ]   ; 4 bytes, signed
```

The TLV's wire-time is the **parent's wire-time + offset**. Use for children inside a timed structured TLV: the parent carries one absolute TS at its level, every child carries a 4-byte delta.

- Range: ±2.147 seconds. Plenty for intra-frame sample timing.
- "Parent" means the wrapping structured TLV's `trailer_ts` (if present), or — if the wrapping structured TLV has no `trailer_ts` — the next outermost ancestor that does.
- A `TF=1` stamp whose ancestor chain has no `trailer_ts` is **anchorless** and names no time. It MUST be rejected with `ERROR{tr::path::invalid}` **by the party that CONSUMES the stamp as a time** — not by the decoder, and not by a relay. See the binding note directly below.

**Where the anchorless-reject MUST binds: at the CONSUMER, never at the decoder and never at a relay** ([#1449](https://github.com/avatarsd-llc/libtracer/issues/1449)). Three obligations, one per role, and they do not overlap:

- **Decode records and succeeds.** `tr::frame` records the relative flag and the delta and returns success. An anchorless `TF=1` frame is a well-formed frame — it decodes cleanly, no TS path raises `PATH_INVALID`, and nothing walks the ancestor chain at decode. That is the specified behaviour, not a shortfall in it.
- **A relay carries it verbatim.** A forwarder re-emits the outer `TS`/`TF` bits and the trailer bytes untouched (§Writer-side status below), on the same relay-opacity precedent as an escape record: a hop that does not read a field does not get to reject on it. This is why the MUST cannot bind at decode — a decoder-side check would make forwarders reject frames they are required to carry opaquely, which is a routing-correctness failure rather than a validation nicety.
- **The consumer rejects.** The party that reads the stamp *as a time* — a resolve walk, a value consumer, an application reading acquisition time off the wire — is the party holding the ancestor chain and the party harmed by a missing anchor, so it is the party that answers `ERROR{tr::path::invalid}`. The reply-echo path declines a `TF=1` root for the same reason: it has no anchor to echo against.

The reference core already implements exactly this split (`fwd_frame_view.hpp`, `op_resolve_walk.hpp`), and a passing test covers it. Earlier revisions of this section read the MUST as a **decoder** obligation and called the codec's non-enforcement a "conformance gap"; that reading was wrong in normative text, and since this page is incorporated in full into [the spec](../spec/v1.md), it is corrected here as an **erratum**. Nothing about the wire surface changes: the same bytes are conforming before and after.

> **`TF=1` is RESERVED grammar, not dead grammar.** No conforming writer mints it today (§Writer-side status below keeps the reference writer gated), but the form stays specified, decodable and relayable: `TF=1` is additive future surface this protocol does not yet use, never surface it removes. Ruled in the 2026-08-20 grilling session and transcribed in [RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md) §4.2.1 (Amendment 1, consequence 4).

### Writer-side status (#1109)

The reference implementation now writes the wire-trailer TS as well as reading it — a plain enhancement, since the trailer above was already fully specified. What ships, and what deliberately does not:

- **Stamping is per-frame opt-in from an INJECTED clock.** `tr::wire::stamp_ts` (`frame.hpp`) sets `opt.TS` and the trailer value together; the clock is the `wire_clock_t` seam the producer supplies — the library never reads ambient time on any frame path. The contract on the value is `CONTEXT.md`'s `origin_timestamp`: per-producer monotonic, wall-clock-advisory. A producer that does not stamp pays nothing — the same shape as the CRC opt-in above.
- **TF=0 only, on purpose.** Wire time is the outermost frame's trailer and is always absolute — [RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md) §4.2.1 makes that the *design*, not a waiting room. The writer additionally stays **gated** on the consumer-side anchor check landing: minting TF=1 while no consumer enforces the anchor rule would produce exactly the near-epoch garbage §pitfalls describes. The byte layout for **both** forms has one home (`wire::store_trailer_ts` / `emit_trailer_ts`), and the frame builders (`stack_writer::header`, the reply emit cursor) accept both forms' TS/TF bits — so the TF=1 writer is a gated follow-up on the anchor check, not a redesign, and the stream shape proposed on #879 reuses this plumbing as-is.
- **A forwarder preserves the stamp verbatim.** The FWD hop rebuild keeps the outer `TS`/`TF` bits and re-emits the trailer bytes, so an origin's stamp survives to the terminus. An inbound **CRC does not cross a hop**: the rebuilt body invalidates it by construction, so `CR` is dropped rather than forwarded stale.
- **The terminus ECHOES a TF=0 request stamp on its reply** — error replies included. This is the ICMP-echo RTT construction: `RTT = origin_now − echoed_stamp`, computed entirely on the origin's clock, no request id, no clock sync, no per-request state. The echo is a capability, not an obligation — a reply without a stamp means "this peer does not echo" and is fully conforming; stored values remain trailer-less at rest (the ADR-0041 trailer-slice is unchanged — only the reply's own outer frame answers a stamp with a stamp).
- **The silent-zero is loud.** `encode` refuses (`empty vector`) a TLV claiming `opt.TS` with no trailer value, or a value whose form contradicts `opt.TF` — it no longer emits a stamp of 1970-01-01. Likewise `wire::emit_tlv`, which writes nothing after the body, clears trailer bits by construction instead of minting a frame that claims a trailer it does not carry.
- **`TIME` (0x0C) is reserved, not forgotten.** Confirmed as the application-domain payload type §below points at; core assigns the code and deliberately neither emits nor consumes it — a TIME body's meaning is the embedder's schema.

### Use case: 1 GS/s ADC with per-sample timing

A tight ADC stream must not spend 8 bytes of timestamp on every slice. It does not — and it does not use the trailer to avoid it either. Per [RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md) §4.2.1 (Amendment 1, 2026-08-21) there are **three clocks with three separate carriers**, and a sample's acquisition time is not one of the trailer's jobs:

```
BATCH (PL=1, user-range record type)   ; one written value, N samples
  trailer_ts (u64): T_tx               ; WIRE time — when this frame left the
                                       ;   interface. OUTERMOST frame only, TF=0.
                                       ;   Not a sample time. One per frame, not per sample.
  ...children:
    TIME  <u64 LE ns>: T_0             ; SAMPLE time of frame 0 — the batch BASE.
                                       ;   One per batch, inside the payload.
    VALUE { sample_0 }                 ; homogeneous children, no trailer of their own
    VALUE { sample_1 }
    ...
```

- **A uniform stream spends 0 bytes per sample on time.** `t(i) = T_0 + i × dt_ns`, where `dt_ns` is the nominal sample period the stream's **descriptor** declares (RFC-0025 §4.3 — a `SETTINGS` LKV beside the data vertex, negotiated once, never repeated per batch). A 1 GS/s capture declares `dt_ns = 1` once and transmits no per-sample timing at all: a 4-byte sample costs 4 bytes.
- **A non-uniform stream (`dt_ns == 0`) carries one packed `i32` offset array**, one signed LE ns offset from `T_0` per sample, in frame order, in a single child — 4 bytes per sample in one contiguous run, decodable in one span, with no per-child TLV header and no anchor walk.
- **The trailer stays wire time.** `T_tx` answers "when did this frame leave an interface", which is a transport-diagnostics and RTT question; conflating it with `T_0` reports every hop's queueing delay as sensor jitter (§pitfalls).

Earlier revisions of this section worked this same use case with an absolute trailer on the parent and a `TF=1` trailer on every child — the shape §"Application-domain timestamps are NOT the wire-trailer TS" below calls a bug, on the very page that prescribes the rule. That contradiction is what this erratum retires ([#1450](https://github.com/avatarsd-llc/libtracer/issues/1450)); the retired shape also cost 4 trailer bytes per sample where the amended one costs 0.

### Application-domain timestamps are NOT the wire-trailer TS

The wire-trailer `TS` is **transport-time**: when the sender put the TLV on the wire. It exists for transport diagnostics, latency measurement, dedup tie-breaking. It is NOT the application-domain timestamp.

Application-domain timestamps (sample acquisition time, sensor exposure window, control deadline) belong inside the payload — typically as a sibling `TIME` TLV inside a wrapping structured TLV (e.g., a user-range record type with PL=1). Three distinct time concerns, three distinct mechanisms:

| Concern | Mechanism |
| ---- | ---- |
| When did the sender put this on the wire? (**WIRE / TX**) | wire-trailer `TS` (this section), outermost frame only, always `TF=0` |
| When was the sample acquired / produced? (**SAMPLE**) | `TIME` (`0x0C`) TLV inside a structured payload — or derived from a batch base plus the descriptor's `dt_ns` |
| When should the consumer present it? (**PLAYOUT**) | *nowhere on the wire* — the receiver derives it from the RTT and clock offset it estimates off read/write carrier echoes |
| When did this vertex last receive a write? | ⚠️ *no mechanism today* — `:liveness.last_seen_ns` is unimplemented ([#586](https://github.com/avatarsd-llc/libtracer/issues/586)) |

Conflating them is a bug; the protocol keeps them separate by construction. This is the three-clock model of [RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md) §4.2.1, and cross-writer *ordering* is none of the three — that is [ADR-0019](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0019-per-producer-monotonic-origin-timestamp.md)'s per-producer monotonic HLC stamp.

---

## Type code registry

| Range | Use | Stability |
| ---- | ---- | ---- |
| `0x00` | Reserved sentinel; never assigned | Forever |
| `0x01` – `0x1F` | Core protocol types ([05-protocol-tlvs.md](05-protocol-tlvs.md)) | Stable; the wire format does not version. |
| `0x20` – `0x7F` | Reserved for future core extensions | Pending registry |
| `0x80` – `0xFF` | User-defined application payload types | No protocol opinion |

Type code `0x00` indicates either a zeroed buffer or framing corruption. Receivers MUST treat `type=0x00` as INVALID.

Type code `0x05` is **reserved** with no assigned meaning (see §rejected designs). Receivers MUST treat `type=0x05` as a reserved-but-unassigned code and apply the rules below; senders MUST NOT emit it.

### Type byte layering

The `type` byte lives at offset 0 of the wire header (L2) but its meaning is L3. A pure-framing parser can route by length+CRC alone; a TLV-aware router uses `type` and `opt.PL` to decide whether to walk into nested children. See [02-graph-model.md](02-graph-model.md) §the six-layer model.

### Versioning and compatibility

**libtracer v1 is the wire format. It does not evolve.** There is no version bit in the header. Future incompatible changes — should they ever be needed — are versioned at the **discovery layer**: a different mDNS service name (`_libtracer-v2._tcp` vs `_libtracer._tcp`), a different default TCP port, a different CAN-ID prefix, etc. Peers learn each other's wire-format identity at discovery time; per-frame versioning is unnecessary and absent.

This is a deliberate design commitment: get the wire format right once. The wire is the most expensive thing to evolve; minimizing its evolution surface forces design rigor here and pushes flexibility into modules, schemas, and the type-code-extension path below.

### Handling unknown type codes

A receiver encountering a TLV whose core-range type code it does not implement — any code not yet assigned in [05-protocol-tlvs.md](05-protocol-tlvs.md), or the reserved `0x05`:

- MUST NOT crash; MUST continue parsing the surrounding stream.
- MUST validate CRC (if present) and respect `length` when skipping over the unknown TLV.
- If the unknown TLV is the outer addressed TLV: respond with `ERROR{tr::schema::type_mismatch}` if a return path exists.
- If nested inside a structured TLV (parent has `opt.PL=1`): treat as opaque bytes and continue.
- Forwarders MAY pass-through unmodified.

This is the **forward extension path**: new core type codes are added in the unassigned core range without breaking deployed receivers, which ignore what they do not understand.

Reserved bits in `opt` non-zero MUST be rejected as INVALID — reserved-bit-non-zero is a hard error to prevent silent semantic drift.

---

## Iterative parsing requirement

Conforming implementations MUST parse nested TLVs (structured TLVs with `opt.PL=1`) iteratively, using an explicit work stack. Recursive parsing is forbidden.

- **Nesting depth**: unbounded by the wire format ([RFC-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md)). **No nesting-depth constant exists at any layer of this protocol.** (Scoped deliberately: this sentence read "no depth constant exists at any layer", which contradicts the **addressing**-layer segment limit that [§addressing](03-addressing.md#path-syntax) and [§`0x06` PATH](05-protocol-tlvs.md) both state — a different axis, on a different layer, and both documents are normatively incorporated by [`v1` §3](../spec/v1.md).) A receiver's actual capability is bounded by its **decode resources**, drawn from the implementation's injected memory seam. Two shapes of exhaustion, both reported as `tr::tlv::nesting_too_deep`: **depth** — one work-stack entry per open level — and **breadth** — a materializing decoder also holds one node per TLV in the frame, so a wide-but-shallow frame can exhaust the same budget. The status name describes the depth case; [RFC-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md) reads it as "exceeds this receiver's decode resources", which covers both. A frame exceeding them MUST be rejected with `ERROR{tr::tlv::nesting_too_deep}` ("exceeds this receiver's decode resources").
- **Work stack size**: an implementation/deployment property (the injected resource), never a protocol constant. Protocol-defined TLV shapes nest ≤ 5 by construction, so every conforming receiver parses every protocol frame at any budget; user-data depth (e.g. a deep branch-write `POINT` tree) is a per-receiver capability of the same kind as maximum frame size.

Rationale: a fixed depth constant exists to guard a *recursive* parser's call stack, and parsing is required to be iterative. The per-level cost is therefore an explicit node already bounded by the injected resource — a 16 KB node rejects what its pool cannot hold, a large host parses arbitrarily deep, and no constant serves both.

### Two parser contexts

The same iterative pattern applies in two distinct contexts; implementations need both:

| Context | Substrate | Cursor advance |
| ---- | ---- | ---- |
| **Wire-receive** | Single contiguous transport buffer | `offset += child_size` within one buffer |
| **In-memory walk** | Rope of views (a chain of refcounted segments) | May step across view boundaries; payload of a single TLV may live in one or several adjacent views |

The wire-receive context applies when a transport module reconstitutes a TLV from a stream. The in-memory walk applies when the router, a subscriber, or a recorder traverses a TLV that was assembled in memory (possibly via mix/split/concat operations) and is no longer flat. See [02-graph-model.md](02-graph-model.md) §Structured TLV as abstraction, memory as rope.

---

## Truncation handling

If wire bytes end before the parser has consumed the full frame, it is a stream-level error:

- Stream transports (TCP, UART, I²C): SHOULD wait for more bytes; report `ERROR{tr::transport::down}` if the stream is closed.
- Datagram transports (UDP, single CAN frame): truncation is `ERROR{tr::frame::truncated}`.

Truncation MUST NOT cause buffer overrun. Implementations MUST validate `length` against available buffer before reading any payload byte.

A header read that succeeds but encounters `length > MAX_TLV_BYTES` (implementation-defined, recommended 16 MiB) MUST be rejected with `ERROR=INVALID` without allocating the segment.

---

## Worked frame examples

### Empty STATUS=OK (minimum frame)

```
09 00 00 00
^  ^  ^^^^^
|  |  length = 0 (u16 LE)
|  opt = 0  (LL=0, no PL/TS/CR)
type = 0x09 STATUS
```

**4 bytes total.** No trailer.

### Single boolean (true), no trailer

```
01 00 01 00 01
^  ^  ^^^^^ ^
|  |  len=1  payload (0x01 = true)
|  opt = 0
type = 0x01 VALUE
```

**5 bytes total.** Header overhead is 4 bytes.

### uint32 with relative TS — the int32-aligned case

A 4-byte payload with 4-byte relative wire-time, no CRC:

```
01 22 04 00 [4 bytes payload] [4 bytes trailer_ts i32]
^  ^  ^^^^^
|  |  length = 4 (u16 LE)
|  opt = 0x22  (TS=1, TF=1)
type = 0x01 VALUE
```

**12 bytes total = three 32-bit aligned chunks** at offsets 0, 4, 8. Naturally aligned for int32 access on every reasonable architecture; no unaligned-load penalty even on the strictest CPUs.

### Same uint32 with absolute TS + CRC-32

```
01 30 04 00 [4 bytes payload] [8 bytes trailer_ts u64] [4 bytes trailer_crc u32]
^  ^  ^^^^^
|  |  length = 4
|  opt = 0x30  (TS=1, CR=1, TF=0, CW=0)
type = 0x01 VALUE
```

**20 bytes total.** Five 32-bit aligned chunks.

### 5-byte VALUE with CRC-32, no TS

```
01 10 05 00 AA BB CC DD EE [4 bytes trailer_crc]
^  ^  ^^^^^ ^^^^^^^^^^^^^^
|  |  len=5  payload
|  opt = 0x10  (CR=1)
type = 0x01 VALUE
```

**13 bytes total.**

### PATH `/sensor/temp` (an opaque TLV holding two packed segment records), outer CRC-32

```
06 10 0C 00 [12 bytes body] [4 bytes trailer_crc]
^  ^  ^^^^^
|  |  length = 12 (sum of two segment records)
|  opt = 0x10  (PL=0, CR=1)
type = 0x06 PATH

  Body (12 bytes) — packed [u8 len][utf8] records:
  06 73 65 6E 73 6F 72            ← len 6, "sensor",  7 bytes
  04 74 65 6D 70                  ← len 4, "temp",    5 bytes
```

**20 bytes total.** A segment record has no header of its own and can carry no trailer; the outer CRC covers the whole body ([RFC-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0018-packed-path-segments.md)).

### Extended-length frame (`LL=1`)

A single VALUE TLV with 100 KiB payload (rare; usually address-shifted):

```
01 18 00 90 01 00 [102400 bytes payload] [4 bytes trailer_crc]
^  ^  ^^^^^^^^^^^
|  |  length = 102400 (u32 LE)
|  opt = 0x18  (CR=1, LL=1)
type = 0x01 VALUE
```

Header is 6 bytes (LL=1); total frame = 6 + 102400 + 4 = 102410 bytes.

---

## Frame size summary

| Configuration | H | T_ts | T_crc | Total overhead |
| ---- | ---- | ---- | ---- | ---- |
| Minimum (LL=0, no trailer) | 4 | 0 | 0 | 4 |
| LL=0, CRC-16 only | 4 | 0 | 2 | 6 |
| LL=0, CRC-32 only | 4 | 0 | 4 | 8 |
| LL=0, TS rel only | 4 | 4 | 0 | 8 |
| LL=0, TS rel + CRC-16 | 4 | 4 | 2 | 10 |
| LL=0, TS rel + CRC-32 | 4 | 4 | 4 | 12 |
| LL=0, TS abs only | 4 | 8 | 0 | 12 |
| LL=0, TS abs + CRC-16 | 4 | 8 | 2 | 14 |
| LL=0, TS abs + CRC-32 (typical wire frame) | 4 | 8 | 4 | 16 |
| LL=1 (extended length), TS abs + CRC-32 | 6 | 8 | 4 | 18 |

Selectable length width is what buys the 4-byte floor: a fixed-u32 length field would put the header minimum at 6 bytes and the typical wire frame at 18. `LL=0` saves 2 bytes on every default frame; `CW=1` saves a further 2 and `TF=1` a further 4, each where the smaller variant fits.

---

## Interop: minimal vs feature-rich implementations

**Every conforming receiver MUST accept all combinations of `LL`, `CW`, `TF`.** Selection is per-TLV; senders may mix variants freely.

A minimal-feature implementation MAY emit only the smaller variants (`LL=0`, `CW=1`, `TF=1` where applicable) for its outgoing TLVs. Because **every receiver MUST accept every variant** (above) and can pre-allocate worst-case, **no per-peer capability negotiation is needed or defined**: a sender simply SHOULD default to the smaller variants and use a larger one only when the smaller would not fit. There are no other negotiable wire features in protocol v1 — reserved bits are frozen, and any wire-incompatible change is a different protocol version selected at the discovery layer. See [ADR-0013](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0013-v1-scope-boundaries.md).

The protocol guarantees: no conforming TLV exceeds the declared bounds (`u32` length, `CRC-32`, `u64` absolute TS). A minimum-feature implementation can pre-allocate worst-case buffers and CRC tables and never encounter a peer that exceeds them.

---

## Pitfalls

Each entry pairs a rule stated above with the failure mode an implementation that misses it produces.

- **No depth constant exists.** An implementation that hardcodes a nesting limit — 32 is the value most often assumed, because it is what a recursive parser needs — rejects frames a conforming peer is entitled to send, and passes its own round-trip tests because its sender never emits one. Bound the work stack by the decode resource actually injected, and report exhaustion as `tr::tlv::nesting_too_deep`.
- **Breadth exhausts the same budget as depth.** A materializing decoder holds one node per TLV, not one per open level. An implementation that budgets only for open levels accepts a wide-but-shallow frame and then runs out of nodes mid-walk, at a point where the payload has already been partly committed.
- **CRC coverage excludes the header.** An implementation that folds `type`, `opt` and `length` into the accumulator interoperates with itself and with nothing else. The symptom is a 100% `crc_fail` rate against a conforming peer, not an intermittent one — an intermittent rate points at the trailer-`TS` fold instead, which is included and must follow the payload bytes in that order.
- **Trailer bytes are not payload bytes.** A forwarder that re-emits the received byte range verbatim carries the upstream's wire-time and a CRC computed over a different span. Strip at ingress, attach fresh at egress; the payload region is what stays byte-identical.
- **Reserved bits are a reject, not a mask.** An implementation that clears bits 7 and 0 before decoding accepts frames a conforming receiver rejects, and absorbs exactly the silent semantic drift the reject exists to prevent. Both bits are frozen for the lifetime of v1, so the check can be hardened at compile time.
- **Wire-trailer `TS` is not acquisition time.** An implementation that surfaces `trailer_ts` to the application as the sample timestamp reports every forwarding hop's queueing delay as sensor jitter. Acquisition time is a `TIME` TLV inside the payload.
- **A relative timestamp without an anchor is an error, not a zero — and the error belongs to the CONSUMER.** `TF=1` whose ancestor chain carries no `trailer_ts` MUST be rejected by whoever reads the stamp as a time. An implementation that defaults the missing anchor to zero produces timestamps that parse, sort and plot — near the Unix epoch. An implementation that instead moves the check into its *decoder* fails the other way: its forwarders reject frames they are required to relay opaquely, turning a timestamp question into a routing outage.
- **`length` is validated before anything is allocated.** An implementation that sizes a buffer from `length` before checking it against the bytes actually available lets a `LL=1` frame claiming 4 GiB in a 60-byte datagram exhaust the receive pool, from an unauthenticated peer.

---

## Rejected designs

For future readers wondering about paths not taken:

- **LEB128 / varint length** — branchy parser, unpredictable payload offset, hostile to streaming and SIMD. Rejected in favor of fixed-width with a single LL bit.
- **Finite-pool length encoding** — a fixed set of length slot-classes on the wire. Rejected in favor of the LL bit; the slot-class concept survives only as a receive-buffer pooling convention internal to the runtime, not on the wire.
- **Variable-width type field / type tree** — would let a router dispatch by content shape without payload parse. Rejected because libtracer routes by **path**, not type; schema is per-vertex (`:schema`); and adding wire-level type-tree encoding fights claim 5 ("the graph imposes no shape on user data"). Self-describing payloads use NAME-tagged children inside a structured TLV (a user-range type code with `PL=1`) instead. Cap'n Proto / FlatBuffers solved the schema-on-the-wire problem already; libtracer is deliberately schema-by-introspection.

- **Generic `LIST` type code** — a generic structured-container type code with no specific semantic. Every structured TLV in the registry has a specific purpose (SUBSCRIBER, POINT, ACL, SETTINGS, STATUS, ERROR); user-defined structured records use user-range type codes (`0x80–0xFF`) with `PL=1`. The `PL` bit alone signals "has nested children"; the type byte tells what those children mean. Type code `0x05` is reserved with no assigned meaning and is not available for reuse (collision-prevention).

- **Per-frame version bit (`VR`)** — bit 7 of `opt` as a version-bump flag. Rejected. The wire format is committed once and not bumped per-frame; future incompatible changes (if ever needed) are versioned at the discovery layer (mDNS service name, port, etc.). The bit stays reserved.
- **Per-TLV priority bits in `opt`** — priority is transport-time and per-link; the subscription's cached priority at L4 covers it ([RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.A). The bits are reclaimed for `LL`/`CW`/`TF` instead.
- **Alignment-promise bit** — modern CPUs handle unaligned loads efficiently; promising alignment requires sender padding, which forces variable framing. Net loss. Rejected.
- **Variable-width TS field beyond {abs-u64, rel-i32}** — exhaustively explored; no third form earns its complexity.
- **u64 length** — intentionally absent. Capping at u32 forces address-shift discipline and protects minimum-impl interop.
