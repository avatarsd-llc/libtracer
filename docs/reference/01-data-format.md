# Reference 01 — Data Format

> **Status**: draft, v0.1, 2026-05-03 (revised same day in design review). Byte-precise definition of every libtracer frame on the wire. A second-implementer SHOULD be able to write an interoperable parser/sender from this section alone.
> **See also**: the implementation rationale (CRC choice, atomic ordering, MCU stack safety) is in [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md). That document predates this revision; the byte layout there is now superseded by what's below.

---

## Frame layout

Every libtracer TLV is a **header + payload + optional trailer**. The payload is a contiguous, untouched user region; metadata never interleaves with it.

```
Offset       Field         Width        Notes
-----------  ------------  -----------  -------------------------------------------
0            type          u8           TLV type code (see §Type code registry)
1            opt           u8           Bit-packed options (see §Options bitfield)
2            length        u32 LE       Payload byte count (fixed 32-bit)
6            payload       length × u8  Pure user region — no metadata interleave
6 + L        trailer_ts    u64 LE       Optional, present iff opt.TS = 1
                                        Wire-time timestamp set by sender at egress;
                                        nanoseconds since Unix epoch
6 + L + 8t   trailer_crc   u32 LE       Optional, present iff opt.CR = 1
                                        CRC-32C over [payload || trailer_ts]
```

Where `L = length` (the value of the length field) and `t = (opt.TS ? 1 : 0)`.

Total frame size: `6 + L + 8·(opt.TS) + 4·(opt.CR)` bytes.

- **Endianness**: little-endian for `length`, `trailer_ts`, `trailer_crc`, and any multi-byte payload field. Matches Cortex-M, ARMv8, x86, ESP32; no per-platform byte swap.
- **Alignment**: the header is **packed**, not naturally aligned. Implementations MUST NOT assume `length` (offset 2) or trailer fields are aligned. Use unaligned loads or memcpy.
- **Minimum frame**: 6 bytes (empty payload, no trailer) — corresponds to e.g. an empty STATUS=OK signal.

### The trailer is append-only at egress, strip-only at ingress

The trailer's purpose is to keep the **payload region invariant** across boundaries. A TLV exists in three states without the payload bytes ever moving:

```
At rest (in graph storage, in a recorder file):
  [ header ] [ payload ]                       ← 6 + L bytes

In transit (on a transport, with optional integrity + wire time):
  [ header ] [ payload ] [ trailer_ts? ] [ trailer_crc? ]
                                               ← 6 + L + 0..12 bytes

At rest again (after received, validated, stripped):
  [ header ] [ payload ]                       ← 6 + L bytes (same payload bytes)
```

Sender at egress: stream payload bytes out, accumulate CRC over them, optionally append `trailer_ts` (and fold those 8 bytes into the CRC), append `trailer_crc`. **No payload reordering.**

Receiver at ingress: read 6-byte header, read `length` payload bytes (DMA / copy / mmap straight into a refcounted segment, accumulating CRC), optionally read `trailer_ts` (folded into CRC), validate `trailer_crc`. **Then publish the segment as a view.** No parsing happens during the payload-bytes copy.

A bridge re-emitting a TLV onto another transport: strip incoming trailer, attach outgoing trailer (fresh wire-time, fresh CRC). Payload bytes never move.

A recorder writing to disk: strip the trailer, store `[ header ][ payload ]`. On replay: re-attach a fresh trailer (replay-time TS, recomputed CRC). Same payload bytes.

This symmetry between TLV-at-rest and TLV-in-transit is the load-bearing benefit of the trailer. It is what makes the same-substrate insight ([02-graph-model.md](02-graph-model.md)) extend cleanly across transport boundaries.

---

## Options bitfield

```
bit 7  6  5  4  3  2  1  0
    +--+--+--+--+--+--+--+--+
    |VR|PL|TS|CR|PR PR R  R |
    +--+--+--+--+--+--+--+--+

VR    Version-bump bit. 0 = wire format v0.1 (this doc). 1 = future major.
PL    Payload-is-LIST bit. 0 = opaque bytes. 1 = payload is concatenated
      child TLVs (the iterative parser recurses one level).
TS    Trailer-has-timestamp. 1 = 8-byte u64 wire-time timestamp present
      after payload. Set by sender at egress; nanoseconds since Unix epoch.
CR    Trailer-has-CRC. 1 = 4-byte CRC-32C present at very end of frame
      (after trailer_ts if both are present).
PR PR Priority hint, bits [3:2], value 0..3 (0=low, 3=critical).
      Routers MAY use this for fast-path dispatch ordering without
      parsing :settings.priority.
R R   Reserved, MUST be zero in v0.1. Receivers MUST reject TLVs with
      non-zero reserved bits as INVALID.
```

The `VR` bit is intentionally first (highest); a parser sees it before any other interpretation kicks in. A v0.2 receiver can immediately distinguish v0.1 traffic.

The `PR` field is a **hint**, not a guarantee. Routers without priority awareness MUST ignore it; routers with priority awareness MAY reorder dispatch but MUST preserve per-publisher ordering of same-priority writes (FIFO within priority class).

### Why `TS` and `CR` are independent flags

- `TS=0, CR=0` — minimal frame, 6-byte header + payload. Use for trusted in-process / SHM transports.
- `TS=0, CR=1` — payload + 4-byte CRC. Use when integrity matters but wire-time doesn't (e.g., a synchronous request/response where elapsed-time is measured by the caller).
- `TS=1, CR=0` — payload + 8-byte wire-time. Use over a trusted SHM transport where you still want latency telemetry. CRC excluded for cycle savings.
- `TS=1, CR=1` — full trailer, 12 bytes overhead. Use for all real network traffic.

Distinct flags let each concern toggle independently without ordering ambiguity (CRC is always at the very end, TS is always before CRC if present).

### Application-domain timestamps are NOT the trailer TS

The wire-trailer `TS` is the **transport-time** the sender put the TLV on the wire. It exists for transport diagnostics, latency measurement, and dedup tie-breaking. It is NOT the application-domain timestamp.

Application-domain timestamps (sample acquisition time, sensor exposure window, control deadline) belong inside the payload — typically as a sibling `TIME` TLV (type `0x0C`, [05-protocol-tlvs.md](05-protocol-tlvs.md)) inside a wrapping `LIST`. Three distinct time concerns, three distinct mechanisms:

| Concern | Mechanism |
| ---- | ---- |
| When did the sender put this on the wire? | `opt.TS=1` trailer |
| When was the sample acquired / produced? | `TIME` TLV inside payload LIST |
| When did this vertex last receive a write? | `:liveness.last_seen_ns` field |

Conflating them is a bug; the protocol keeps them separate by construction.

---

## Length encoding

The `length` field is **fixed-width unsigned 32-bit little-endian**, encoding the **payload byte count**. It does NOT include the header itself, the trailer, or its own bytes.

- Range: 0 to 2^32 − 1 (≈ 4 GiB). The protocol places no smaller cap; in practice payloads of a few MiB are the design ceiling, with anything larger expressed as address-shift slicing across `ep[0..N]` (see [03-addressing.md](03-addressing.md) §address-shift slicing).
- Maximum recommended single-TLV payload: 1 MiB. Above that, slice via address-shift. The cap is convention, not protocol enforcement.

### Why fixed-width, not LEB128

The earlier draft used LEB128 (variable-width). Reverted because:

- **Branchless parse** — receiver reads exactly 4 bytes at offset 2, no decode loop, no continuation-bit checks. Predictable for SIMD and cycle-bound MCU loops.
- **Streaming-friendly** — receiver knows the full payload extent immediately and can allocate / DMA the entire payload region without inspecting any of its bytes.
- **No-fragmentation principle already caps practical size** — anything large goes through address-shift, so "encode payloads up to 2^63" was a feature LEB128 bought that we don't use.
- **Cost is bounded** — the worst case (vs LEB128) is +3 bytes per small TLV. For a 1-byte boolean with full trailer, total frame is 18 bytes vs the LEB128-era 15. Acceptable; amortizes immediately as soon as payload exceeds ~12 bytes.

The previous "finite-pool length" mode (`FP` bit) is **removed**. Its purpose was to avoid the LEB128 decode loop on tiny MCUs; with fixed u32 length there's nothing to avoid. The pool-class concept survives as an internal **receive-buffer pooling convention** (which slot to allocate from based on declared length) — a runtime/allocator concern, not a wire-format concern.

---

## CRC-32C

The integrity check is **CRC-32C** (Castagnoli polynomial `0x1EDC6F41`, reflected representation `0x82F63B78`), present in the trailer iff `opt.CR = 1`.

- **Initial value**: `0xFFFFFFFF`.
- **Final XOR**: `0xFFFFFFFF`.
- **Coverage**: the **payload bytes** plus, if `opt.TS = 1`, the **`trailer_ts` bytes** (in that order). The header (`type`, `opt`, `length`) is NOT included in the CRC.
- **Field width on the wire**: 32 bits, little-endian.
- **Position**: always at the very end of the frame (offset `6 + L + 8·(opt.TS)`).

When `opt.CR = 0`, the trailer_crc field is **absent** (not present-but-zero — actually missing from the byte stream). The frame is shorter by 4 bytes.

When `opt.CR = 1`, the receiver MUST validate and MUST reject mismatches with `ERROR=CRC_FAIL` (see [05-protocol-tlvs.md](05-protocol-tlvs.md) §error codes).

### Why CRC at the end, not the start

- **Streaming send**: the sender writes the header, then streams payload bytes out while accumulating the CRC. At the end of the payload, append optional trailer_ts (folding it into the CRC), then append the final CRC. **No backward seek to fill in a CRC field at the start.**
- **Streaming receive**: the receiver reads the header, then streams payload bytes directly into the destination segment while accumulating the CRC. Reads optional trailer_ts (folds into CRC). Compares trailer_crc against accumulator. **No "skip the CRC field while computing CRC" footgun** that front-loaded layouts require (with the CRC field at offset 2, the sender has to compute CRC over the rest of the frame, then go back and write the result; the receiver has to skip those 4 bytes during accumulation).
- **At-rest = in-transit symmetry** — the trailer is purely a transport-level annotation. Stripping it leaves the payload region byte-identical to the in-graph storage form.

### Why CRC covers payload + trailer_ts (not payload alone)

If `trailer_ts` is included in the CRC, a corrupted timestamp shows up as `CRC_FAIL` rather than as a silent bogus time. The cost is a few extra accumulator updates (8 bytes folded in after the payload loop) — trivially cheap. The benefit is no silent TS corruption.

If you genuinely need CRC over payload-only (e.g., recording the CRC value to disk and then re-emitting with a different TS), set `opt.TS = 0` on the recorded TLV; on replay, the recorder re-attaches a fresh TS and recomputes CRC.

### Hardware acceleration paths

- **x86-64 SSE 4.2**: `_mm_crc32_u8` / `_mm_crc32_u64` intrinsics (since 2008).
- **ARMv8 `+crc`**: `__crc32cb` / `__crc32cw` / `__crc32cx` from `<arm_acle.h>` (since 2014).
- **Cortex-M (no CRC instruction)**: software CRC-32C with a 256-entry lookup table runs at ~2 cycles/byte. Acceptable for the wire rates Cortex-M devices speak.

### Optional CRC-16 mode

A `LIBTRACER_CRC_16` build-time option swaps `trailer_crc:u32` for `trailer_crc:u16` (CRC-16-CCITT, polynomial `0x1021`), saving 2 bytes per TLV with CRC. **Wire break**: TLVs from a `_CRC_16` build are NOT interoperable with default builds. Implementations supporting this mode MUST clearly identify themselves at discovery time.

---

## Type code registry

| Range | Use | Stability |
| ---- | ---- | ---- |
| `0x00` | Reserved (never assigned — collision protection / sentinel) | Forever |
| `0x01` – `0x1F` | Core protocol types ([05-protocol-tlvs.md](05-protocol-tlvs.md)) | Stable; version-bump required to change |
| `0x20` – `0x7F` | Reserved for future core extensions | Pending IETF-style registry; request via PR until then |
| `0x80` – `0xFF` | User-defined application payload types | No protocol opinion; sender and receiver must agree out-of-band |

Type code `0x00` is permanently unassigned: a frame starting with `0x00` indicates either a zeroed buffer (sentinel for "no TLV here") or framing corruption. Receivers MUST treat `type=0x00` as INVALID and discard the frame.

The full byte-precise spec for each core type is in [05-protocol-tlvs.md](05-protocol-tlvs.md).

### Where the type byte sits in the layering

The `type` byte lives in the wire header (Layer 0) but its meaning is a Layer 1 concern. See [02-graph-model.md](02-graph-model.md) §the four-layer model. A pure-framing parser can route by `length + CRC` alone; a TLV-aware router uses `type` (and `opt.PL`) to decide whether to walk into nested children.

### Forward / backward compatibility

A v0.1 receiver encountering a TLV with a type code in the **reserved-but-unassigned** range (`0x0E` – `0x7F` as of v0.1):

- MUST NOT crash; MUST continue parsing the surrounding stream.
- MUST validate the CRC (if `opt.CR = 1`) and respect the `length` field when skipping over the unknown TLV.
- If the unknown TLV is the **outer** addressed TLV (top-level on the wire), respond with `ERROR=TYPE_MISMATCH` to the sender if a return path exists.
- If the unknown TLV is **nested** inside a LIST, treat as opaque bytes and continue.
- If acting as a router/bridge, MAY pass-through unmodified.

A v0.1 receiver encountering `opt.VR = 1`:

- MUST send `ERROR=VERSION_MISMATCH` back to the sender.
- MUST NOT attempt to interpret the payload.
- MUST NOT crash.

A v0.1 receiver encountering an unknown reserved-bit setting in `opt` (`R` bits non-zero):

- MUST reject the TLV as INVALID. Reserved-bit-non-zero is a hard error to prevent silent semantic drift across versions.

A future v1.0 implementation MAY speak v0.1 (with `opt.VR = 0`) by restricting itself to v0.1 type codes and option semantics, gated by per-peer negotiation. The negotiation mechanism is deferred to v1.0 design.

---

## Iterative parsing requirement

Conforming implementations MUST parse nested TLVs (LIST containers) **iteratively**, using an explicit work stack. Recursive parsing is forbidden.

- **Maximum nesting depth**: 32. TLVs nested deeper MUST be rejected with `ERROR=NESTING_TOO_DEEP`.
- **Work stack size**: bounded by the depth limit; a 32-frame stack with 32 bytes per frame fits in 1 KiB.

Rationale: MCU stacks are small (4 KiB on STM32F4 default; 8 KiB on ESP32 default). A maliciously deep nesting on the wire could overflow a recursive parser's call stack with no recovery.

The reference iterative pattern is given in [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) §iterative parser pattern. (That snippet predates the header-layout revision; offsets in the parser will need to update from `8 + L + length` to `6 + length + trailer`. The control flow is unchanged.)

---

## Truncation handling

If the wire bytes end before the parser has consumed the full frame (`6 + length + 8·(opt.TS) + 4·(opt.CR)` bytes), the parser MUST treat this as a stream-level error:

- For a stream transport (TCP, UART, I²C), the parser SHOULD wait for more bytes if the underlying transport indicates more are coming, or report `ERROR=TRANSPORT_DOWN` if the stream is closed.
- For a datagram transport (UDP, single CAN frame), truncation in the datagram itself is `ERROR=TRUNCATED`.

Truncation MUST NOT cause a buffer overrun on the receiver side. Implementations MUST validate that the declared `length` does not exceed the available buffer length before reading any payload byte.

A header read that succeeds but encounters `length > MAX_TLV_BYTES` (implementation-defined, recommended 16 MiB) MUST be rejected with `ERROR=INVALID` without allocating the segment. This protects against a malicious or corrupted header naming a multi-gigabyte length on a constrained receiver.

---

## Worked frame examples

### Empty STATUS=OK (the smallest valid TLV)

```
09 00 00 00 00 00
^  ^  ^^^^^^^^^^^
|  |  length = 0 (u32 LE)
|  opt = 0  (no PL, no TS, no CR, priority 0, reserved 0)
type = 0x09 STATUS
```

**6 bytes total.** No trailer.

### Single boolean (`true`) with full trailer

```
01 30 00 00 00 01 01 [ts:8]                 [crc:4]
^  ^  ^^^^^^^^^^^ ^  ^^^^^^                  ^^^^^
|  |  length = 1   |  trailer_ts (8 bytes)    trailer_crc (4 bytes)
|  |               payload byte = 0x01 (true)
|  opt = 0x30 (TS=1, CR=1, priority 0)
type = 0x01 VALUE
```

`6 (header) + 1 (payload) + 8 (TS) + 4 (CRC) = 19 bytes.`

### 5-byte VALUE with CRC, no timestamp

```
01 10 05 00 00 00 AA BB CC DD EE [crc:4]
^  ^  ^^^^^^^^^^^ ^^^^^^^^^^^^^^  ^^^^^
|  |  length = 5  payload          trailer_crc
|  opt = 0x10 (CR=1)
type = 0x01 VALUE
```

`6 + 5 + 4 = 15 bytes.`

### LIST containing two NAME TLVs (a PATH `/sensor/temp`)

```
06 50 1A 00 00 00 [crc:4]
^  ^  ^^^^^^^^^^^
|  |  length = 26 (sum of both child TLVs' total sizes)
|  opt = 0x50 (PL=1, CR=1)
type = 0x06 PATH

  Inner LIST contents (26 bytes):
  02 00 06 00 00 00 73 65 6E 73 6F 72       ← NAME "sensor", 12 bytes
  ^  ^  ^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^
  |  |  length = 6  "sensor"
  |  opt = 0 (no trailer on inner — outer CRC covers the bytes)
  type = 0x02 NAME

  02 00 04 00 00 00 74 65 6D 70             ← NAME "temp", 10 bytes
```

Outer total: `6 (outer header) + 26 (inner LIST) + 4 (outer CRC) = 36 bytes`.

Inner TLVs typically don't carry their own trailer — the outer LIST's CRC covers the whole concatenated content. Setting `opt.CR=1` on a nested TLV is permitted but redundant unless the inner TLV is meant to be split out and re-routed independently (e.g., by a bridge that strips and re-emits children individually).

### Empty STATUS=OK used as unsubscribe sentinel

Same 6 bytes as the first example, written to `/path:subscribers[N]` to clear slot N.

---

## Frame size summary

| Configuration | Bytes overhead (excluding payload) |
| ---- | ---- |
| Minimum (no trailer) | 6 |
| TS only | 14 |
| CRC only | 10 |
| TS + CRC (typical wire frame) | 18 |

For comparison: the previous LEB128-with-front-CRC layout had 7-byte minimum overhead and required CRC always present in the header (4 bytes whether validated or not). The new layout is 1 byte smaller at the floor (6 vs 7), 3 bytes larger when both trailer fields are used (18 vs 15), and pays the cost only when the trailer's annotations are actually needed.
