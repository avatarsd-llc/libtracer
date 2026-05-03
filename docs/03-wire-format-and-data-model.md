# 03 — Wire Format and Data Model

> **Status**: draft, v0.1, 2026-05-03 — promoted to "stable" at end of week 2 of [doc 02](02-roadmap-weeks-1-to-8.md). Until then, byte positions may shift.
> **Audience**: anyone implementing a parser/serializer; anyone porting libtracer to a new language; anyone debugging on the wire with `tcpdump`.
> **Reading time**: ~25 min.

---

## How to read this doc

§[Header layout](#header-layout) is the byte-exact frame structure. §[Type codes](#type-codes) is the registry. §[Variable-width integers](#variable-width-integers-leb128--finite-pool) is the length encoding. §[CRC-32C](#crc-32c) is the integrity check. §[The same-substrate insight](#the-same-substrate-insight) is the load-bearing architectural claim. §[Iterative parser pattern](#iterative-parser-pattern-mandatory) is the MCU-stack-safety mandate. §[Refcount memory ordering](#refcount-memory-ordering) is the C23 atomics spec. §[Concrete hex examples](#concrete-hex-examples) walks through every core type code with real bytes.

**What this doc deliberately does NOT contain**: fragmentation rules (see [doc 04](04-graph-and-endpoint-api.md) — handled at the addressing layer via address-shift slicing); transport-level framing (see [doc 05](05-modules-transport-and-discovery.md) — each transport handles its own MTU and reassembly).

---

## Header layout

Every libtracer TLV starts with a fixed-position **header**, followed by a variable-length **payload**.

```
Offset  Field        Width    Notes
------  -----------  -------  -----------------------------------------------
0       type         u8       TLV type code (see §Type codes)
1       opt          u8       Bit-packed options (see §Options bitfield)
2       crc          u32      CRC-32C over the payload (see §CRC-32C)
6       length       varint   Payload length in bytes (see §Variable-width
                              integers); 1–9 bytes wide, default LEB128
6+L     payload      []u8     Length bytes of payload data
```

`L` is the encoded width of the length field (1–9 bytes for LEB128; fixed 1/2/4/8 bytes in finite-pool mode).

**Endianness**: little-endian for `crc` and any multi-byte payload field. Matches Cortex-M, ARMv8, x86, ESP32 native; no per-platform swap needed.

**Alignment**: the header is **packed**, not aligned. C23 declaration uses `[[gnu::packed]]` (or `__attribute__((packed))` for older compilers) on `struct tlv_header`. Implementations MUST NOT assume natural alignment for the `crc` or `length` fields.

### Options bitfield

```
bit 7  6  5  4  3  2  1  0
    +--+--+--+--+--+--+--+--+
    |VR|PL|TS|FP|CR|R |R |R |
    +--+--+--+--+--+--+--+--+

VR  Version-bump bit. 0 = wire format v0.1 (this doc). 1 = future major.
PL  Payload-is-LIST bit. 0 = opaque bytes. 1 = nested TLVs (see §Same-substrate).
TS  Timestamp-present bit. If 1, payload begins with an 8-byte u64 timestamp
    (ns since Unix epoch), followed by the actual data. See §Timestamps.
FP  Finite-pool length bit. 0 = LEB128 length. 1 = fixed length per
    `pool-class` field (see §Variable-width integers). Set at compile time
    in `LIBTRACER_FINITE_POOL` builds; unset on hosts that speak both.
CR  CRC-present bit. 0 = `crc` field is zero, not validated. 1 = `crc` is
    valid, parser must verify or reject.
R   Reserved, MUST be zero in v0.1.
```

The `VR` bit is intentionally first: a parser sees the version bit before any other interpretation kicks in, so a v0.2 receiver can immediately distinguish v0.1 traffic and refuse, downgrade, or upgrade as it chooses.

---

## Type codes

Type code allocation:

| Range | Use | Stability |
| ---- | ---- | ---- |
| `0x00` | Reserved (never assigned — collision protection) | Forever |
| `0x01` – `0x1F` | Core protocol types (this doc) | Stable, version-bump required to change |
| `0x20` – `0x7F` | Reserved for future core extensions | Pending registry — request via PR |
| `0x80` – `0xFF` | User-defined application payload types | No protocol opinion |

### Core type codes (carried from [libtracer/tlv.h](../libtracer/tlv.h))

| Code | Name | Payload shape | Purpose |
| ---- | ---- | ---- | ---- |
| `0x01` | `VALUE` | opaque bytes | Endpoint data — sensor reading, control input, anything user-defined |
| `0x02` | `NAME` | UTF-8 bytes (no NUL terminator on the wire) | A path component or vertex name |
| `0x03` | `DESCRIPTION` | UTF-8 bytes | Human-readable description for introspection |
| `0x04` | `SUBSCRIBER` | LIST containing `PATH`, `SETTINGS`, optional `ACL` | Subscription record (written into `ep:subscribers[N]`) |
| `0x05` | `LIST` | sequence of nested TLVs | Container — the graph-node-as-TLV mechanism |
| `0x06` | `PATH` | LIST of `NAME` TLVs | Hierarchical address |
| `0x07` | `POINT` | LIST of `NAME`, `DESCRIPTION`, `SETTINGS`, `SUBSCRIBER` entries | Endpoint definition |
| `0x08` | `ERROR` | u8 error code (see error table below) | Inline error |
| `0x09` | `STATUS` | LIST of `ERROR` and `DESCRIPTION` | Communication status; empty payload = OK |
| `0x0A` | `ACL` | LIST of capability TLVs | Access control list (semantics in [doc 06](06-modules-executor-security-gui.md)) |
| `0x0B` | `SETTINGS` | LIST of named-value TLVs | QoS knobs (RELIABILITY, DURABILITY, etc.; see [doc 04](04-graph-and-endpoint-api.md)) |
| `0x0C` | `TIME` | u64 (8 bytes) | Absolute timestamp, ns since Unix epoch |
| `0x0D` | `ROUTER` | LIST | Router/bridge vertex definition |

(The current [libtracer/tlv.h](../libtracer/tlv.h) ID `STATUS = 0x09` and `tracer.hpp`'s `status_t::ID = tlv_t::PATH` are inconsistent — week 1 of [doc 02](02-roadmap-weeks-1-to-8.md) fixes the C++ side.)

### Error codes (payload of `ERROR`)

```
0x00  OK                   Operation succeeded (rarely sent — empty STATUS implies OK)
0x01  NOT_FOUND            Path does not resolve to a vertex
0x02  PERMISSION_DENIED    ACL rejected the operation
0x03  INVALID_PATH         Malformed PATH or non-UTF-8 NAME
0x04  TYPE_MISMATCH        Payload type incompatible with endpoint schema
0x05  CRC_FAIL             Wire CRC did not match
0x06  VERSION_MISMATCH     VR bit set higher than receiver supports
0x07  BACKPRESSURE         Subscriber queue full; sample dropped per QoS
0x08  TIMEOUT              No response within deadline
0x09  TRANSPORT_DOWN       Underlying transport disconnected
0x0A  SCHEMA_NOT_FOUND     ep:schema read on a vertex that exposes no schema
0x0B  ADDRESS_SHIFT_GAP    Missing index in an address-shift group at deadline
0x0C  – 0x7F  reserved
0x80  – 0xFF  user-defined
```

---

## Variable-width integers (LEB128 + finite pool)

The `length` field uses LEB128 (Little-Endian Base-128, same as DWARF/protobuf varint) **by default**. This makes single-byte TLVs cheap and large TLVs paid-as-you-grow.

### LEB128 encoding rules

Each byte: 7 data bits + 1 continuation bit (high bit). The data bytes are emitted least-significant-first.

```
Length value         Encoded bytes (hex)
0                    00
1                    01
127                  7F
128                  80 01           (low 7 bits = 0, high 7 bits = 1)
1024                 80 08
65535                FF FF 03
1048575              FF FF 3F
2147483647 (i32 max) FF FF FF FF 07
```

Maximum width: 9 bytes (encodes up to 2^63-1 — sufficient for any reasonable TLV; the length field itself is u64 internally).

### Finite-pool mode (`FP` bit)

For tiny MCU builds where the LEB128 decode loop is too expensive (typical Cortex-M0 estimate: 30 cycles for a 3-byte varint vs 4 cycles for a fixed `u16`), libtracer supports a compile-time **finite-pool mode**.

Set `LIBTRACER_FINITE_POOL=ON` at build time. Then the `FP` bit in `opt` is set on every emitted TLV, and the `length` field becomes a single byte naming a **pool class**:

```
length byte value    Slot width      Maximum payload
0x00                 8 bytes         8
0x01                 32 bytes        32
0x02                 128 bytes       128
0x03                 512 bytes       512
0x04                 2 KB            2048
0x05                 8 KB            8192
0x06                 32 KB           32768
0x07                 128 KB          131072
0x08 – 0xFF          reserved
```

The actual payload is allocated from a pre-sized pool of the named class. Wasted space inside the slot is acceptable; the trade is determinism, not bytes.

**Interop**: a host that compiled with `LIBTRACER_FINITE_POOL=OFF` (default) MUST accept TLVs with `FP=1`, decoding by reading the pool-class table. A host that compiled with `LIBTRACER_FINITE_POOL=ON` MUST accept LEB128 TLVs from peers, decoding into the smallest fitting pool slot. A host with `LIBTRACER_FINITE_POOL=ONLY` (post-MVP, real cost-constrained MCU) MAY refuse non-finite-pool peers — this is documented as a non-interop build.

---

## CRC-32C

The integrity check is **CRC-32C** (Castagnoli polynomial `0x1EDC6F41`, reverse representation `0x82F63B78`) over the **payload bytes**, computed with initial value `0xFFFFFFFF`, finalized by XOR with `0xFFFFFFFF`.

### Why CRC-32C, not the current XOR-16

The XOR-16 in current [libtracer/tlv_vector.hpp](../libtracer/tlv_vector.hpp) is **a parity check, not a CRC**. It detects single-bit flips and some 2-bit flips, but a transposed pair of bytes XORs to the same value. For a wire protocol crossing CAN/I2C/UART/IP, this is unacceptable.

CRC-32C provides:
- Detection of all single-bit, double-bit, and odd-bit error patterns up to the polynomial degree.
- Detection of all burst errors ≤ 32 bits.
- ~`1/2^32` false-positive rate for arbitrary corruption.
- Hardware acceleration on **x86** (SSE 4.2, `crc32` instruction since 2008) and **ARMv8** (`crc32x` instruction since 2014). Cortex-M lacks the hardware instruction but software CRC-32C runs at ~2 cycles/byte with a 256-entry lookup table, ~5 cycles/byte without. Acceptable.

### Software fallback

A 256-entry table-driven implementation:

```c
#include <stdint.h>
#include <stddef.h>

static const uint32_t crc32c_table[256] = { /* generated, see below */ };

uint32_t crc32c(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc = crc32c_table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}
```

Table generation (compile-time `constexpr` in C23, executed once at build time):

```c
constexpr uint32_t crc32c_init_entry(uint32_t i) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++) {
        c = (c & 1u) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
    }
    return c;
}
// fill crc32c_table[i] = crc32c_init_entry(i) for i in 0..255 at compile time
```

### Hardware acceleration

On x86-64 with SSE 4.2:
```c
#include <nmmintrin.h>
uint32_t crc32c_hw(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len >= 8) { crc = _mm_crc32_u64(crc, *(const uint64_t *)data); data += 8; len -= 8; }
    while (len--)    { crc = _mm_crc32_u8(crc, *data++); }
    return crc ^ 0xFFFFFFFFu;
}
```

On ARMv8 with the `+crc` extension: use `__crc32cb` / `__crc32cw` / `__crc32cx` intrinsics from `<arm_acle.h>`.

### CRC-16-CCITT fallback for footprint-constrained builds

A `LIBTRACER_CRC_16` build option swaps the `crc:u32` field for `crc:u16` (saving 2 bytes per TLV) using CRC-16-CCITT (`0x1021`). This is a wire break: builds with `LIBTRACER_CRC_16` are not interoperable with default builds. Documented; opt-in only.

---

## The same-substrate insight

This is the load-bearing technical claim of libtracer.

**A TLV in memory IS a graph node IS the wire bytes.**

In most middleware:
- The wire encoding is one representation (CDR, Protobuf, Cap'n Proto, Zenoh's `z_encoding`).
- The in-memory message struct is another (decoded fields).
- The routing topology graph is a third (separate metadata).

In libtracer, all three collapse into one. The mechanism: **buffer chains of views over real memory.**

### How nested TLVs work

When the `PL` (payload-is-LIST) bit is set in the header `opt` byte, the payload is interpreted as a sequence of child TLVs concatenated end-to-end. Each child has its own 8-byte header and varint length, and may itself have `PL=1` for further nesting.

```
Outer TLV (LIST, PL=1):
  +-----------+--------+--------+--------+
  | type=0x05 | opt=PL | crc    | length |  header (8 bytes + varint)
  +-----------+--------+--------+--------+
  | inner TLV 1: header + payload         |
  +---------------------------------------+
  | inner TLV 2: header + payload         |
  +---------------------------------------+
  | inner TLV 3: header + payload         |
  +---------------------------------------+
```

This list IS the graph node. To walk a vertex's children: parse the LIST, iterate the inner TLVs, recurse into any with `PL=1`.

### Why this is zero-copy

Underneath, each "inner TLV" is represented as a **buffer view**: a struct holding `{owner, offset, length}` where `owner` is a refcounted pointer to the real memory backing the buffer. The graph "contains" inner TLVs by holding views into the parent's memory.

```
Real memory (received from socket):
  [TCP recv buffer; 4 KB; refcount=1]
   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Outer LIST view:
  { owner = recv_buffer_refcount,
    offset = 0,
    length = 1024 }

Inner TLV 1 view:
  { owner = recv_buffer_refcount,  // same backing memory
    offset = 8 + len_of_outer_length_field,
    length = inner_1_length }

Inner TLV 2 view:
  { owner = recv_buffer_refcount,  // same backing memory
    offset = 8 + len_of_outer_length_field + 8 + ... ,
    length = inner_2_length }
```

Operations on the graph that look like data manipulation (split a list into two, concatenate two lists, insert a new child, slice off the trailing N children) **do not move bytes**. They construct new view structs whose `owner` field bumps the refcount of the underlying buffer.

**Spec-level proof obligation**: any sequence of mix/split/concat operations, followed by a `serialize_to_wire()` walk, MUST produce the same bytes as if the corresponding mutations had been applied to a fresh buffer. (Test: `tests/test_substrate_invariant.c` in week 1 of [doc 02](02-roadmap-weeks-1-to-8.md).)

### Ownership transfer at endpoint delivery

When a transport module receives bytes from the wire, it constructs a top-level buffer view over the received memory and hands it to the router via `router_dispatch(buffer_view)`. The router walks the view tree, finds the destination endpoint, and delivers the view to the endpoint's queue.

**At delivery, the buffer view's ownership is "transferred" to the endpoint** — meaning the endpoint takes the existing refcount, no new copy is made. The transport module relinquishes its reference (refcount decrement; the buffer survives because the endpoint now holds the count).

If multiple subscribers are attached, the view is **cloned** (refcount bumped per subscriber, no byte-level copy). Each subscriber sees the same backing memory through its own view struct.

**This is the mechanism that makes "as fast as Cap'n Proto with pub/sub semantics" credible.**

### Reference: prior art in the repo

The C++ buffer-segment exploration in [Docs/drafts/Write custom buffer_segment class.md](../Docs/drafts/Write custom buffer_segment class.md) is the design reference for this mechanism, with `std::shared_ptr` / `std::weak_ptr` as the C++ refcount primitive. The C23 implementation uses hand-rolled atomic refcount per [§Refcount memory ordering](#refcount-memory-ordering) below.

---

## Iterative parser pattern (mandatory)

Cortex-M class MCUs have small stacks (4 KB on STM32F4, 8 KB on ESP32 default). Recursive parsing of nested LIST TLVs is dangerous: a maliciously deep or accidentally deep nesting blows the stack with no recourse.

**libtracer implementations MUST parse nested TLVs iteratively, using an explicit work queue.**

Reference pattern:

```c
typedef struct {
    const uint8_t *data;
    size_t        offset;
    size_t        end;
    void         *user_ctx;  // caller-supplied per-frame context
} tlv_parse_frame_t;

#define TLV_MAX_DEPTH 32

int tlv_parse_iter(const uint8_t *buf, size_t len,
                   int (*on_tlv)(const tlv_t *t, int depth, void *ctx),
                   void *ctx) {
    tlv_parse_frame_t stack[TLV_MAX_DEPTH];
    int               sp = 0;

    stack[sp++] = (tlv_parse_frame_t){ buf, 0, len, ctx };

    while (sp > 0) {
        tlv_parse_frame_t *f = &stack[sp - 1];
        if (f->offset >= f->end) { sp--; continue; }

        const tlv_t *t   = (const tlv_t *)(f->data + f->offset);
        size_t       tlen = tlv_total_size(t);  // header + length-field + payload
        if (f->offset + tlen > f->end) return TLV_ERR_TRUNCATED;

        if (on_tlv(t, sp, f->user_ctx) != 0) return TLV_ERR_CALLBACK_ABORT;

        f->offset += tlen;

        if ((t->header.opt & TLV_OPT_PL) && sp < TLV_MAX_DEPTH) {
            stack[sp++] = (tlv_parse_frame_t){
                .data     = tlv_payload_ptr(t),
                .offset   = 0,
                .end      = tlv_payload_len(t),
                .user_ctx = f->user_ctx,  // or per-frame derived
            };
        } else if (t->header.opt & TLV_OPT_PL) {
            return TLV_ERR_NESTING_TOO_DEEP;
        }
    }
    return 0;
}
```

`TLV_MAX_DEPTH = 32` is enforced. Adversarial input cannot blow the stack; deeply nested intent is rejected with `ERR_NESTING_TOO_DEEP`.

---

## Refcount memory ordering

The buffer-view refcount is the hottest atomic in the system. Get it wrong and you get use-after-free under SMP. Get it too strong and you eat a memory barrier on every increment.

The canonical pattern (Boost intrusive_ptr style), in C23 `<stdatomic.h>`:

```c
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    atomic_uint_least32_t refcount;
    void (*destroy)(void *);
    // ... actual buffer fields follow ...
} segment_t;

// Increment: relaxed.
//
// The caller already holds a reference (otherwise it could not validly
// reach `s`), so the data dependency travels via that existing reference.
// No new synchronization is needed when bumping the count.
static inline void seg_ref_inc(segment_t *s) {
    atomic_fetch_add_explicit(&s->refcount, 1, memory_order_relaxed);
}

// Decrement: acq_rel.
//
//   release  — all writes through this reference are flushed before the
//              decrement is visible to other threads, so when someone
//              else observes count=1 (we were last) they see a fully-
//              consistent buffer state.
//   acquire  — when WE observe the count drop to 1 (we were last), we
//              synchronize with all prior releases from other threads,
//              so the destructor sees the final state.
static inline void seg_ref_dec(segment_t *s) {
    if (atomic_fetch_sub_explicit(&s->refcount, 1,
                                  memory_order_acq_rel) == 1) {
        s->destroy(s);  // we were the last reference
    }
}

// Read-only inspection (debug, metrics): acquire.
//
// Pairs with the release on every dec; ensures a consistent snapshot.
// Don't use this for logic decisions — by the time you act on the value
// it may have changed. For "am I the last holder?" use `seg_ref_dec`.
static inline uint32_t seg_ref_load(const segment_t *s) {
    return atomic_load_explicit(&s->refcount, memory_order_acquire);
}

// Weak-to-strong upgrade (for "weak segment" equivalent of weak_ptr).
//
//   success: acq_rel — same logic as inc + sync with last-decrementer.
//   failure: acquire — re-load with acquire so the retry sees the
//                      latest count, including a possible drop to 0.
static inline bool seg_weak_upgrade(segment_t *s) {
    uint32_t cur = atomic_load_explicit(&s->refcount,
                                        memory_order_acquire);
    while (cur > 0) {
        if (atomic_compare_exchange_weak_explicit(
                &s->refcount, &cur, cur + 1,
                memory_order_acq_rel, memory_order_acquire)) {
            return true;
        }
        // cur is updated by CAS on failure; loop retries
    }
    return false;  // object is gone
}
```

### `LIBTRACER_NO_ATOMIC` mode

For Cortex-M0/M0+ (no LDREX/STREX) and bare-metal single-threaded contexts, set `-DLIBTRACER_NO_ATOMIC=ON` at build time. The atomics collapse to plain `uint32_t`:

```c
#ifdef LIBTRACER_NO_ATOMIC
typedef struct {
    uint32_t refcount;     // not atomic — single-threaded only
    void (*destroy)(void *);
    // ...
} segment_t;

static inline void seg_ref_inc(segment_t *s) { s->refcount++; }
static inline void seg_ref_dec(segment_t *s) {
    if (--s->refcount == 0) s->destroy(s);
}
// ...
#endif
```

**Caller contract** when `LIBTRACER_NO_ATOMIC` is set: the application must guarantee no cross-thread sharing of segments. Single-threaded MCU firmware satisfies this trivially. Multi-threaded code with manual mutex-protected handoff between threads is technically correct but brittle; do not recommend.

---

## Versioning

Wire format major version is in the `VR` bit of `opt`. v0.1 = `VR=0`. A future v1.0 = `VR=1`.

There is no minor-version field. **Any change to type-code semantics, header layout, or option bit meaning is a major bump.** New type codes in the `0x20–0x7F` reserved range can be added without a major bump (older receivers ignore unknown codes — see below).

### Forward compatibility

A v0.1 receiver encountering a TLV with a type code in `0x20–0x7F` that it doesn't recognize:
- MUST NOT crash.
- MUST report `ERROR=0x04 TYPE_MISMATCH` if the TLV was addressed to it.
- MAY pass-through the TLV unmodified if it's a router/bridge.
- MUST validate CRC and respect the `length` field when skipping.

A v0.1 receiver encountering a TLV with `VR=1` (future major):
- MUST send back `ERROR=0x06 VERSION_MISMATCH`.
- MUST NOT attempt to interpret the payload.

### Backward compatibility

A future v1.0 implementation **MAY** speak v0.1 by clearing the `VR` bit and using only v0.1 type codes / option semantics, gated by per-peer negotiation (negotiation mechanism deferred to v1.0 design).

---

## Timestamps

Two ways to attach a timestamp to a TLV:

### (a) Header `TS` bit + 8-byte u64 prefix in payload

```
opt.TS = 1
payload = [u64 timestamp (ns since Unix epoch, little-endian)] || [actual data]
```

Cheap (8 bytes), positionally fixed (decode-without-parse). Use this for the common case.

### (b) Sibling `TIME` TLV inside a LIST

```
LIST {
  TIME { u64 ns since Unix epoch }
  VALUE { payload bytes }
}
```

More flexible (multiple timestamps per TLV — sample time vs send time vs receive time). Use this when more than one time matters, or when the TLV is inside a LIST anyway.

### Time domain

Both forms are nanoseconds since Unix epoch (1970-01-01 00:00:00 UTC). Wraparound is in year 2554 (u64 ns ≈ 584 years). Negative timestamps (pre-epoch) are not representable; reject as `ERROR=0x03 INVALID_PATH`.

PTP-synced clocks are recommended on networks where coherency matters (see [doc 04](04-graph-and-endpoint-api.md) §coherency). On networks without PTP, accuracy is whatever NTP gives you (~1 ms typical).

### Coherency across address-shift slices

When a logical message is sliced across `ep[0..N]` (see [doc 04](04-graph-and-endpoint-api.md) §address-shift), every slice MUST carry the **same timestamp**. The subscriber assembles slices by `(timestamp, index)` pair; missing index in a timestamp group = loss.

---

## Concrete hex examples

Small TLVs walked through byte by byte. All multi-byte fields are little-endian.

### Example 1 — Empty TLV

A `STATUS=OK` (empty payload) without timestamp or CRC:

```
01 00 00 00 00 00 00
^  ^  ^^^^^^^^^^^ ^
|  |  |           length = 0 (1 LEB128 byte)
|  |  crc = 0 (CR=0, so unverified)
|  opt = 0
type = 0x09 STATUS  ← wait, 0x01 is VALUE, not STATUS
```

Corrected — STATUS empty:

```
09 00 00 00 00 00 00
^  ^  ^^^^^^^^^^^ ^
|  |  |           length = 0
|  |  crc field = 0 (CR bit unset)
|  opt = 0
type = 0x09 STATUS
```

7 bytes total. The smallest meaningful libtracer TLV.

### Example 2 — RC car control (5 bytes payload, with CRC)

A `VALUE` TLV carrying 5 bytes (`AA BB CC DD EE`) with CRC-32C verified:

```
01 10 5C E1 E5 03 05 AA BB CC DD EE
^  ^  ^^^^^^^^^^^ ^  ^  ^^^^^^^^^^^
|  |  |           |  |  payload (5 bytes)
|  |  |           |  length = 5 (1 byte LEB128)
|  |  crc-32c     |
|  |  = 0x03E5E15C (computed over the 5 payload bytes)
|  opt = 0x10 (CR bit set, no other flags)
type = 0x01 VALUE
```

Total: 12 bytes on the wire. Header overhead: 12 - 5 = 7 bytes.

### Example 3 — RC car control with timestamp

Same as Example 2 plus an 8-byte ns timestamp via the `TS` bit:

```
01 30 [crc] 0D [ts:8] AA BB CC DD EE
^  ^  ^     ^  ^      ^^^^^^^^^^^^^^
|  |  |     |  |      payload (5 bytes)
|  |  |     |  timestamp (u64, ns since epoch)
|  |  |     length = 13 (1 + 5 = wait, no: 8 + 5 = 13)
|  |  crc-32c over the entire 13-byte payload (timestamp + data)
|  opt = 0x30 (CR=1, TS=1)
type = 0x01 VALUE
```

Total: 7 + 13 = 20 bytes on the wire.

### Example 4 — A LIST containing two NAME TLVs (a two-deep PATH)

A `PATH` `/sensor/temp`:

```
06 40 [crc] 14
   |  |     |
   |  |     length = 20 bytes (the inner LIST)
   |  crc over the inner list bytes
   opt = 0x40 (PL=1, payload-is-LIST)
type = 0x06 PATH

  Inner LIST contents (20 bytes):
  02 00 00 00 00 00 00 06 73 65 6E 73 6F 72   ← NAME "sensor", 6 bytes
  ^  ^  ^^^^^^^^^^^ ^  ^^^^^^^^^^^^^^^^^^^
  |  |  |           |  payload "sensor" (6 bytes)
  |  |  |           length = 6
  |  |  crc = 0 (CR=0 to keep the example short)
  |  opt = 0
  type = 0x02 NAME

  02 00 00 00 00 00 00 04 74 65 6D 70           ← NAME "temp", 4 bytes
```

Outer total: 7 (header) + 20 (payload) = 27 bytes.

### Example 5 — A SUBSCRIBER record written into `ep:subscribers[0]`

A subscription with PATH `/log/output` and SETTINGS `reliability=reliable`:

```
04 40 [crc] [length] {
  06 40 [crc] [len] { NAME "log", NAME "output" }   ← PATH
  0B 40 [crc] [len] { NAME "reliability", VALUE 0x01 }   ← SETTINGS
}
```

This entire SUBSCRIBER TLV is what the publisher writes via:

```c
tracer_write("/log/output:subscribers[0]", subscriber_tlv);
```

The presence of the SUBSCRIBER TLV at that path causes the router to fan out future writes to `/log/output` to whatever transport+address pair the SUBSCRIBER's PATH resolves to. (See [doc 04](04-graph-and-endpoint-api.md) for the full subscription mechanics.)

---

## Wire-level framing vs application-level slicing

These are **two distinct concerns** that the spec keeps separate.

### Wire-level framing — handled by transport modules

- TCP carries a byte stream; the transport module reads `[8 bytes header] [length field] [length payload bytes]` and reconstructs one TLV at a time.
- UDP carries datagrams; one TLV per datagram if it fits the MTU; if not, **the application MUST use address-shift slicing** rather than relying on UDP's IP-level fragmentation (which is widely broken across firewalls).
- CAN carries 8-byte frames (CAN-FD: 64-byte); the `transport_can` module fragments and reassembles a TLV across multiple CAN frames using CAN ID + sequence-number conventions documented in [doc 05](05-modules-transport-and-discovery.md).
- I2C/SPI/UART carry byte streams (I2C with start/stop framing); same approach as TCP.

**Wire-level framing is the transport module's responsibility. Senders never see it. The libtracer TLV is the unit at the API.**

### Application-level slicing — `ep[0..N]` address-shift

When the application has a **logically-large** payload (e.g., a 10 MB camera frame) and wants subscribers to consume it in slices (e.g., process row by row, or stream to disk without buffering the whole thing), the **publisher** addresses the slices to enumerated child endpoints `/camera/frame/[0]`, `/camera/frame/[1]`, ... `/camera/frame/[N]` with the same timestamp on each.

The subscriber sees N independently-delivered TLVs, can act on each immediately, and uses the shared timestamp + index to detect missing slices.

**This is not fragmentation.** Each slice is a complete, valid, independently-routable TLV. The transport module sees N TLVs and ships them however the transport ships them. There is no reassembly state to lose.

[doc 04](04-graph-and-endpoint-api.md) specifies the subscriber's view of address-shift slicing and the QoS knobs that govern wait-for-completion vs deliver-as-arrive semantics.

---

## What's NOT in this doc

- The C ABI for the parser/serializer functions — see the header file `libtracer/core/tlv.h` once it's written in week 1 of [doc 02](02-roadmap-weeks-1-to-8.md).
- The router / endpoint / subscription mechanics — see [doc 04](04-graph-and-endpoint-api.md).
- Transport-module ABI (the contract a transport-side module exports to the core) — see [doc 05](05-modules-transport-and-discovery.md).
- Discovery / scouting — see [doc 05](05-modules-transport-and-discovery.md).
- Security / encryption / authentication — see [doc 06](06-modules-executor-security-gui.md). The wire format is security-agnostic; security wraps it at the transport layer.
- Negotiation between v0.1 and future major versions — deferred to v1.0 design.
