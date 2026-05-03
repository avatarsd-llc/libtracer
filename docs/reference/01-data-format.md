# Reference 01 — Data Format

> **Status**: draft, v0.1, 2026-05-03 (revised twice in design review same day). Byte-precise definition of every libtracer frame on the wire. A second-implementer SHOULD be able to write an interoperable parser/sender from this section alone.
> **See also**: design rationale (CRC choice, atomic ordering, MCU stack safety) is in [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md). That document predates this revision; the byte layout there is superseded.

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

A bridge re-emitting on another transport: strip incoming trailer, attach outgoing trailer (fresh wire-time, fresh CRC). A recorder writing to disk: strip the trailer, store header+payload. On replay: re-attach a fresh trailer. **Payload bytes are invariant under every transition.**

This symmetry is what makes the same-substrate insight extend cleanly across multi-hop bridging and recording. See [02-graph-model.md](02-graph-model.md) §the trailer enables payload-bytes invariance.

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

### Default vs extended forms

The default header is **4 bytes**. A typical small TLV has no trailer (4-byte total) or a small trailer (CRC-16 only, 6 bytes total). The extended forms (`LL=1`, full TS, full CRC) are opt-in and pay only for what they buy.

### Why no priority bits

An earlier draft put a 2-bit priority field in `opt`. Removed. Priority is a **transport-time, per-link, non-coherent** concern; the L2 header should carry coherent things or things every router must see. Per-TLV priority bits buy nothing that `:settings.priority` cached at the bridge doesn't already cover. See [02-graph-model.md](02-graph-model.md) §the six-layer model — priority lives at L4.

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

(Earlier drafts used LEB128 + a "finite-pool" mode. Both removed — see §rejected designs at end.)

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

Both variants are mandatory for conforming receivers. A receiver that sees `CR=1` MUST verify whichever variant `CW` selects; mismatches return `ERROR=CRC_FAIL`.

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
- A TLV with `TF=1` whose ancestor chain has no `trailer_ts` MUST be rejected with `ERROR=INVALID_PATH` (the relative timestamp is meaningless without an anchor).

### Use case: 1 GS/s ADC with per-sample timing

Without relative TS, a tight ADC stream would carry an 8-byte timestamp on every slice — bandwidth waste. With relative TS:

```
USER_SAMPLE_RECORD (PL=1) {            ; outer carries absolute TS in trailer
  trailer_ts (u64): T_window_start
  ...children:
    VALUE { sample_0, trailer_ts (i32): +0   }
    VALUE { sample_1, trailer_ts (i32): +1ns }
    ...
}
```

A 4-byte sample with i32 relative TS in trailer = 12 bytes total, three int32 chunks at 4-byte aligned offsets (`type,opt,length`, `payload`, `trailer_ts`) — see worked examples below.

### Application-domain timestamps are NOT the wire-trailer TS

The wire-trailer `TS` is **transport-time**: when the sender put the TLV on the wire. It exists for transport diagnostics, latency measurement, dedup tie-breaking. It is NOT the application-domain timestamp.

Application-domain timestamps (sample acquisition time, sensor exposure window, control deadline) belong inside the payload — typically as a sibling `TIME` TLV inside a wrapping structured TLV (e.g., a user-range record type with PL=1). Three distinct time concerns, three distinct mechanisms:

| Concern | Mechanism |
| ---- | ---- |
| When did the sender put this on the wire? | wire-trailer `TS` (this section) |
| When was the sample acquired / produced? | `TIME` TLV inside a structured payload |
| When did this vertex last receive a write? | `:liveness.last_seen_ns` field |

Conflating them is a bug; the protocol keeps them separate by construction.

---

## Type code registry

| Range | Use | Stability |
| ---- | ---- | ---- |
| `0x00` | Reserved sentinel; never assigned | Forever |
| `0x01` – `0x1F` | Core protocol types ([05-protocol-tlvs.md](05-protocol-tlvs.md)) | Stable; the wire format does not version. |
| `0x20` – `0x7F` | Reserved for future core extensions | Pending registry |
| `0x80` – `0xFF` | User-defined application payload types | No protocol opinion |

Type code `0x00` indicates either a zeroed buffer or framing corruption. Receivers MUST treat `type=0x00` as INVALID.

Type code `0x05` is **retired** (was `LIST` in earlier drafts; see §rejected designs). Receivers MUST treat `type=0x05` as a reserved-but-unassigned code and apply the rules below; senders MUST NOT emit it.

### Type byte layering

The `type` byte lives at offset 0 of the wire header (L2) but its meaning is L3. A pure-framing parser can route by length+CRC alone; a TLV-aware router uses `type` and `opt.PL` to decide whether to walk into nested children. See [02-graph-model.md](02-graph-model.md) §the six-layer model.

### Versioning and compatibility

**libtracer v0.1 is the wire format. It does not evolve.** There is no version bit in the header. Future incompatible changes — should they ever be needed — are versioned at the **discovery layer**: a different mDNS service name (`_libtracer-v2._tcp` vs `_libtracer._tcp`), a different default TCP port, a different CAN-ID prefix, etc. Peers learn each other's wire-format identity at discovery time; per-frame versioning is unnecessary and absent.

This is a deliberate design commitment: get the wire format right once. The wire is the most expensive thing to evolve; minimizing its evolution surface forces design rigor here and pushes flexibility into modules, schemas, and the type-code-extension path below.

### Handling unknown type codes

A receiver encountering a TLV with a type code in `0x0E – 0x7F` (reserved-but-unassigned, including the retired `0x05`):

- MUST NOT crash; MUST continue parsing the surrounding stream.
- MUST validate CRC (if present) and respect `length` when skipping over the unknown TLV.
- If the unknown TLV is the outer addressed TLV: respond with `ERROR=TYPE_MISMATCH` if a return path exists.
- If nested inside a structured TLV (parent has `opt.PL=1`): treat as opaque bytes and continue.
- Routers/bridges MAY pass-through unmodified.

This is the **forward extension path**: new core type codes can be added in `0x0E – 0x7F` without breaking existing receivers. Receivers gracefully ignore what they don't understand.

Reserved bits in `opt` non-zero MUST be rejected as INVALID — reserved-bit-non-zero is a hard error to prevent silent semantic drift.

---

## Iterative parsing requirement

Conforming implementations MUST parse nested TLVs (structured TLVs with `opt.PL=1`) iteratively, using an explicit work stack. Recursive parsing is forbidden.

- **Maximum nesting depth**: 32. Deeper TLVs MUST be rejected with `ERROR=NESTING_TOO_DEEP`.
- **Work stack size**: bounded by depth limit; ~1 KiB.

Rationale: MCU stacks are small (4 KiB on STM32F4 default). Maliciously deep nesting on the wire could overflow a recursive parser's call stack with no recovery.

### Two parser contexts

The same iterative pattern applies in two distinct contexts; implementations need both:

| Context | Substrate | Cursor advance |
| ---- | ---- | ---- |
| **Wire-receive** | Single contiguous transport buffer | `offset += child_size` within one buffer |
| **In-memory walk** | Rope of views (a chain of refcounted segments) | May step across view boundaries; payload of a single TLV may live in one or several adjacent views |

The wire-receive context applies when a transport module reconstitutes a TLV from a stream. The in-memory walk applies when the router, a subscriber, or a recorder traverses a TLV that was assembled in memory (possibly via mix/split/concat operations) and is no longer flat. See [02-graph-model.md](02-graph-model.md) §LIST as abstraction, memory as rope.

---

## Truncation handling

If wire bytes end before the parser has consumed the full frame, it is a stream-level error:

- Stream transports (TCP, UART, I²C): SHOULD wait for more bytes; report `ERROR=TRANSPORT_DOWN` if the stream is closed.
- Datagram transports (UDP, single CAN frame): truncation is `ERROR=TRUNCATED`.

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

### PATH `/sensor/temp` (a structured TLV containing two NAME children), outer CRC-32

```
06 50 12 00 [18 bytes children] [4 bytes trailer_crc]
^  ^  ^^^^^
|  |  length = 18 (sum of two child TLVs)
|  opt = 0x50  (PL=1, CR=1)
type = 0x06 PATH

  Children (18 bytes):
  02 00 06 00 73 65 6E 73 6F 72   ← NAME "sensor", 10 bytes (no trailer)
  02 00 04 00 74 65 6D 70           ← NAME "temp",  8 bytes (no trailer)
```

**26 bytes total.** Inner NAMEs carry no trailer; outer CRC covers their bytes.

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

For comparison: the previous fixed-u32-length-only revision had 6-byte header minimum and 18-byte typical-wire overhead. The new selectable-width design saves 2 bytes on every default-LL frame, plus an additional 2/4 bytes via CRC-16 / TS-relative when chosen.

---

## Interop: minimal vs feature-rich implementations

**Every conforming receiver MUST accept all combinations of `LL`, `CW`, `TF`.** Selection is per-TLV; senders may mix variants freely.

A minimal-feature implementation MAY emit only the smaller variants (`LL=0`, `CW=1`, `TF=1` where applicable) for its outgoing TLVs. A feature-rich implementation communicating with a minimal peer MAY restrict its outgoing TLVs to the smaller variants based on per-peer capability discovery (mechanism deferred to v1.0). Until then, feature-rich implementations SHOULD emit smaller variants by default and use the larger variants only when the smaller would not fit.

The protocol guarantees: no conforming TLV exceeds the declared bounds (`u32` length, `CRC-32`, `u64` absolute TS). A minimum-feature implementation can pre-allocate worst-case buffers and CRC tables and never encounter a peer that exceeds them.

---

## Rejected designs

For future readers wondering about paths not taken:

- **LEB128 / varint length** — branchy parser, unpredictable payload offset, hostile to streaming and SIMD. Rejected in favor of fixed-width with a single LL bit.
- **Finite-pool length encoding** — was a wire-format mode in an earlier draft. Replaced by the LL bit; the slot-class concept survives only as a receive-buffer pooling convention internal to the runtime, not on the wire.
- **Variable-width type field / type tree** — would let a router dispatch by content shape without payload parse. Rejected because libtracer routes by **path**, not type; schema is per-vertex (`:schema`); and adding wire-level type-tree encoding fights claim 5 ("the graph imposes no shape on user data"). Self-describing payloads use NAME-tagged children inside a structured TLV (a user-range type code with `PL=1`) instead. Cap'n Proto / FlatBuffers solved the schema-on-the-wire problem already; libtracer is deliberately schema-by-introspection.

- **Generic `LIST` type code (`0x05`)** — earlier drafts had a generic structured-container type code with no specific semantic. Removed. Every structured TLV in the registry has a specific purpose (SUBSCRIBER, PATH, POINT, ROUTER, ACL, SETTINGS, STATUS, ERROR); user-defined structured records use user-range type codes (`0x80–0xFF`) with `PL=1`. The `PL` bit alone signals "has nested children"; the type byte tells what those children mean. Type code `0x05` is permanently retired (collision-prevention) — never reused.

- **Per-frame version bit (`VR`)** — earlier drafts had bit 7 of `opt` as a version-bump flag. Removed. The wire format is committed once and not bumped per-frame; future incompatible changes (if ever needed) are versioned at the discovery layer (mDNS service name, port, etc.). The bit becomes one more reserved.
- **Per-TLV priority bits in `opt`** — priority is transport-time and per-link; cached `:settings.priority` at L4 covers it. The bits are reclaimed for `LL`/`CW`/`TF` instead.
- **Alignment-promise bit** — modern CPUs handle unaligned loads efficiently; promising alignment requires sender padding, which forces variable framing. Net loss. Rejected.
- **Variable-width TS field beyond {abs-u64, rel-i32}** — exhaustively explored; no third form earns its complexity.
- **u64 length** — intentionally absent. Capping at u32 forces address-shift discipline and protects minimum-impl interop.
