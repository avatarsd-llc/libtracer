# libtracer v0.1 — Implementation Analysis (Revision 1)

> **Author**: independent review, 2026-05-06.
> **Method**: five iterative analysis cycles over the v0.1 reference suite plus the existing code stubs. Each cycle uses the prior cycle's findings as input and probes deeper.
> **Inputs**:
> - All twelve reference docs ([../reference/](../reference/))
> - Normative spec [docs/spec/v1.md](../spec/v1.md)
> - Prior architecture review [docs/plans/98-architecture-review.md](../plans/98-architecture-review.md)
> - Vision doc [docs/plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md)
> - Code stubs: [core/include/libtracer/tlv.h](../../core/include/libtracer/tlv.h), [core/include/libtracer/tracer.hpp](../../core/include/libtracer/tracer.hpp), [core/include/libtracer/tlv_vector.hpp](../../core/include/libtracer/tlv_vector.hpp), [core/include/libtracer/tlv_string.hpp](../../core/include/libtracer/tlv_string.hpp), [core/include/libtracer/serdes.h](../../core/include/libtracer/serdes.h), [bindings/rust/src/lib.rs](../../bindings/rust/src/lib.rs), [bindings/typescript/src/index.ts](../../bindings/typescript/src/index.ts)

---

## Context

You asked me to analyse the v0.1 protocol you are drafting and "figure out what blind spots and misalignments exist," "review the code," "figure out what important questions [you] missed," and "do this in iteration cycles like a real developer." You also asked me to think about "how it will look like in the implementation in different languages."

The v0.1 reference suite is exceptionally well structured. The prior architecture review ([../plans/98-architecture-review.md](../plans/98-architecture-review.md)) already names five blockers, eight blind spots, and a P0/P1/P2/P3 fixlist — that is a high bar. The job of this document is **not to repeat what you already caught**. It is to surface what that review did not, with the same level of scepticism and a focus on:

1. **Concrete contradictions inside the spec** that a second implementer would hit.
2. **Code-vs-spec drift** in the existing C/C++ core stubs.
3. **Cross-language portability traps** — places where the C-shaped abstractions fight Rust/Go/TS/Python.
4. **Adversarial / corner cases** that aren't called out.
5. **Architectural choices and where they leak** — not as criticism, as engineering rigour.
6. **Questions you haven't asked but a senior implementer would.**

Each finding ends with a proposed solution. The fixes are mostly additive (new text, new fields, new modules) — the architecture itself is sound. The riskiest concrete finding is that **the existing C/C++ core code is the v0.0 sketch and is wire-incompatible with the v0.1 spec on almost every dimension**; if you ship the spec while leaving that code, your reference impl fails its own conformance test on day 1.

---

## Methodology — five cycles

| Cycle | Question | Mode |
| ---- | ---- | ---- |
| 1 | Are the docs self-consistent? Does the wire layout add up? Does code match docs? | Static analysis. Bytes, bits, opt fields, registry entries, EBNF. |
| 2 | What does this look like in C++/Rust/Go/TS/Python? Where do the C-shaped contracts fight other languages? | Cross-language portability. |
| 3 | What breaks under stress, ambiguity, malice, or operator error? | Adversarial / corner case. |
| 4 | Why is each load-bearing decision the way it is? Where does it leak? | Architectural rationale. |
| 5 | What hasn't been asked but should be? | Unasked questions. |

Each cycle's findings feed forward; cycle 5's questions are partly the residue of unresolved items from cycles 1–4.

---

## Cycle 1 — Static analysis (internal consistency + code drift)

### 1.1 The PATH TLV `opt` byte example is wrong (bona-fide spec bug)

[../reference/01-data-format.md](../reference/01-data-format.md#L62-L82) defines the `opt` byte as:

```
bit 7  6  5  4  3  2  1  0
    R  PL TS CR LL CW TF R
```

So `PL` is bit 6, `CR` is bit 4. `0x40 = PL only`. `0x50 = PL + CR`.

Now:
- [../reference/01-data-format.md §worked frame examples PATH](../reference/01-data-format.md#L353-L364) shows `06 50 12 00` and labels `opt = 0x50 (PL=1, CR=1)` with an outer trailer_crc — internally consistent.
- BUT [../reference/05-protocol-tlvs.md §PATH §static / pre-encoded byte literal](../reference/05-protocol-tlvs.md#L267-L283) shows `06 50 12 00` and explicitly labels `0x50 = PL=1 (bit 6) only, no TS, no CR, LL=0` — **this is mathematically wrong**. Bit 4 is set in `0x50`.
- The same wrong literal is repeated in [../reference/03-addressing.md §static path handles](../reference/03-addressing.md#L321-L326) and [../reference/06-user-data-packing.md §recipe — single sensor](../reference/06-user-data-packing.md#L502-L506) ("22 bytes of flash. Zero RAM.").

Either the bit table is wrong (PL is bit 4? bit 5?) or the byte literal is `06 40 12 00`. Looking at every other `opt` value in the suite (`0x10` for CR-only, `0x18` for CR+LL, `0x22` for TS+TF, `0x30` for TS+CR), the bit table is the source of truth and the **PATH literal should be `06 40 12 00`, 22 bytes total** (header + two NAME children, no inner trailers).

**Severity**: high. A second implementer copying the byte literal into `.rodata` produces a CR-bit-set PATH TLV with no trailer_crc — receivers will read past `length` bytes looking for a 4-byte CRC and either misframe or reject as `CRC_FAIL`.

**Fix**: change `0x50 → 0x40` in every PATH byte literal across 03/05/06; recompute `length = 18` (already correct, sum of two NAME children with no inner trailer); leave 01-data-format.md's PATH example with `0x50` if it intends an outer CRC, but make the comment match.

### 1.2 The C/C++ core is wire-incompatible with the v0.1 spec on almost every axis

This is the single most important static-analysis finding. The repo's `core/include/libtracer/*.h*` files are v0.0-shaped:

| Aspect | v0.1 spec | Code in `tlv.h` / `tlv_vector.hpp` / `tlv_string.hpp` |
| ---- | ---- | ---- |
| Header size | 4 bytes default (LL=0), 6 bytes (LL=1) | **8 bytes** (`resize(8)` everywhere; `header_t = {type:u8, opt:u8, crc:u16, length:u32}` in [tlv.h:44-49](../../core/include/libtracer/tlv.h#L44-L49)) |
| Length field | u16 (default) or u32 (LL=1), trailer-positioned | **u32 only**, in header |
| CRC | trailer-positioned, CRC-32C default or CRC-16-CCITT (CW bit) | **header-positioned, u16 XOR-16** ([tlv_vector.hpp:192-207](../../core/include/libtracer/tlv_vector.hpp#L192-L207)) |
| Wire-time TS | optional trailer, u64 abs or i32 rel | **absent** |
| `opt` bits | 6 named (PL/TS/CR/LL/CW/TF) + 2 reserved | **2 named (TIMESTAMP/CRC) + 6 reserved** ([tlv.h:35-39](../../core/include/libtracer/tlv.h#L35-L39)) |
| `LIST = 0x05` | **retired**; receivers must skip; senders must not emit | **still defined and used as the structured-container default** ([tlv.h:24](../../core/include/libtracer/tlv.h#L24)) |
| NAME payload | UTF-8 bytes, **no NUL terminator** | **always includes NUL** ([tlv_string.hpp:31, :47-50, :62-65](../../core/include/libtracer/tlv_string.hpp#L30-L65)) |
| Path handles | required at API; `.rodata` PATH TLV literal | **string-form everywhere; no path handle abstraction** |
| API primitives | `read`/`write`/`await` only | `point_i::connect/disconnect` ([tracer.hpp:82-83](../../core/include/libtracer/tracer.hpp#L82-L83)) — directly violates load-bearing claim 2 |
| Decode model | TLV-as-cast (zero-copy view tree) | `serdes_i::serialize/deserialize` ([serdes.h:13-17](../../core/include/libtracer/serdes.h#L13-L17)) — explicit decode step the spec rejects |
| TLV ownership | view over a refcounted segment | `tlv_vector_t : public std::vector<uint8_t>` — owns its bytes, heap-allocated, public-inherits a stdlib container (antipattern) |
| C++ flags | `-fno-exceptions`, `-fno-rtti` per README | `tlv_vector.hpp` throws `std::runtime_error` ([tlv_vector.hpp:134, :184](../../core/include/libtracer/tlv_vector.hpp#L134); also [tlv_string.hpp:78, :87](../../core/include/libtracer/tlv_string.hpp#L78)) |
| C++ compiles | — | `tracer.hpp` declares `class name_t` three times (template at [:22-28](../../core/include/libtracer/tracer.hpp#L22-L28), non-template-NAME at [:31-34](../../core/include/libtracer/tracer.hpp#L31-L34), non-template-DESCRIPTION at [:37-40](../../core/include/libtracer/tracer.hpp#L37-L40)) — won't compile |

**Severity**: blocker. Conformance vectors (referenced from `docs/spec/v1.md §4`) cannot pass against this code. The Rust binding is empty (`#![no_std]` only); the TS binding is empty (one constant). There is effectively no v0.1 reference implementation yet — only v0.1 docs and v0.0 code.

This isn't a criticism; v0.0 → v0.1 is a deliberate wire break (R5 in [../plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md#L147)). The action item is to **schedule the rewrite explicitly**, before any "draft → frozen" promotion happens.

**Fix**: rewrite `core/include/libtracer/*` from scratch against the v0.1 spec:
- New `tlv.h` with the 4/6-byte header (`type:u8, opt:u8, length:u16`, optional `length_hi:u16` when LL=1), no in-header CRC, no in-header TS.
- Drop `tlv_vector.hpp`, `tlv_string.hpp`, `serdes.h` entirely; replace with `view.h` (the L1 view + segment + rope), `tlv_cast.h` (in-place accessors), `path_handle.h` (the `.rodata` literal macro).
- New `tracer.h` (NOT `.hpp` — keep the public ABI C-callable; C++ wrapper goes in a separate header) exposing only `tracer_read/write/await(handle, view)`.
- Delete `point_i::connect/disconnect`. The `point_i` virtual interface itself is suspect — if every vertex has the same contract, virtuals impose vtable cost in flash.
- Reconcile the README's "no-RTTI, no-exceptions" claim with the implementation discipline. Either change the README or remove the throws.

The order this should happen is non-obvious; see §"Recommended fix sequence" at the end.

### 1.3 The `tracer.hpp` skeleton has duplicate class names

[tracer.hpp:22-40](../../core/include/libtracer/tracer.hpp#L22-L40) declares three classes literally named `name_t`:

```cpp
template <typename T>
class name_t : public serdes_i { /* VALUE — yes, this is named name_t */ };

class name_t : public std::string, serdes_i { /* NAME */ };
class name_t : public std::string, serdes_i { /* DESCRIPTION */ };
```

This is a stub; it doesn't compile. Worth flagging because it indicates the C++ skeleton hasn't been built or run since being written — there's no CI sentinel catching this. (The library builds on "trust the headers compile" but they don't.)

**Fix**: in the rewrite, name them per their TLV: `value_t<T>`, `name_t`, `description_t`, etc. Add a `cmake -B build && cmake --build build` step to CI so basic compilation is enforced before conformance is even attempted.

### 1.4 Reserved-bit semantics across MUSTs

[../reference/01-data-format.md §options bitfield](../reference/01-data-format.md#L66-L82) says:

> `R` Reserved (bit 7). MUST be zero. Receivers MUST reject non-zero as INVALID.
> `R` Reserved (bit 0). MUST be zero. Receivers MUST reject non-zero as INVALID.

But [../reference/01-data-format.md §forward extension path](../reference/01-data-format.md#L237-L247) says:

> A receiver encountering a TLV with a type code in `0x0E – 0x7F` ... MUST NOT crash; MUST continue parsing the surrounding stream.

The two rules clash if a future v0.1 minor revision wanted to use a reserved `opt` bit for something benign. The "reject as INVALID" door is closed; future evolution must repurpose a type code, not an opt bit. That's intentional design rigour — but it should be *explicitly stated* that **reserved bits are forever reserved**, not "for now."

**Fix**: add a sentence to §options bitfield: "Reserved bits are committed for the lifetime of v0.1 wire format; future incompatible use lives at the discovery layer per §versioning. The `0x0E–0x7F` type-code range is the only forward-extension path within v0.1."

### 1.5 Wildcards in path EBNF aren't grammatical

[../reference/03-addressing.md §path syntax](../reference/03-addressing.md#L11-L23) defines:

```
segment     = name [ index ]
name        = 1*64 ( UTF8-codepoint - reserved )
reserved    = "/" / ":" / "." / "[" / "]" / "*" / "?"
```

That excludes `*` from `name`. Then [§wildcards](../reference/03-addressing.md#L102-L126) introduces `*`, `**`, `[*]` as valid in subscriber paths. The grammar above doesn't admit them; the wildcard syntax is bolted on.

**Severity**: low (parser implementations will figure it out), but it's the kind of EBNF gap a conformance vector author will trip over.

**Fix**: extend the EBNF to a `subscriber-path` non-terminal:

```
subscriber-path = root *subscriber-segment [ field-sep field-chain ]
subscriber-segment = segment-sep ( segment / "*" / "**" )
```

and clearly label the original `path` non-terminal as the "concrete path" used for `read`/`write`/`await`, with `subscriber-path` as the form valid only inside SUBSCRIBER's PATH child.

### 1.6 ROUTER's child layout uses an unusual NAME-tag-then-value convention

[../reference/05-protocol-tlvs.md §ROUTER payload](../reference/05-protocol-tlvs.md#L583-L601) shows:

```
ROUTER (PL=1) {
  NAME "origin_peer_id"   VALUE <16 bytes>
  NAME "origin_timestamp" TIME  <u64 ns>
  NAME "hop_count"        VALUE <u8>
  ...
  NAME "data"             <wrapped TLV>
}
```

A parser walking ROUTER's children sees a sequence of TLVs — each NAME and each value is a sibling at the same level. Pairing them up requires the parser to know "ROUTER's children are tag/value pairs," which is application logic, not pure structural parsing. This is unlike SETTINGS, which has the same convention but at least has a documented field schema.

This is workable but worth contrasting with the alternative (a structured ROUTER_FIELD type code with one TLV per metadata pair, recursive parse). The current convention is denser on the wire (no per-field wrapper) at the cost of bespoke pairing logic.

**Bigger concern**: a corrupted ROUTER could have an odd number of children (NAME without paired value), or two NAMEs in a row, or a NAME at the end with no value. What's the conformance behaviour? Not specified. A bridge that stores partial metadata and continues is one option; one that rejects with `STATUS=INVALID` is another.

**Fix**: add a normative subsection to ROUTER stating: "ROUTER's children MUST appear as alternating NAME/value pairs, terminated by `NAME 'data'` followed by exactly one wrapped TLV. A receiver encountering odd parity, missing 'data' tag, or two consecutive NAMEs MUST reject with `STATUS=ERROR(INVALID)`." Choose a code; INVALID isn't in the registry yet — see §1.10.

### 1.7 Trailer behaviour for nested TLVs is underspecified

[../reference/01-data-format.md](../reference/01-data-format.md) and [../reference/02-graph-model.md](../reference/02-graph-model.md) consistently say "the trailer is append-only at egress, strip-only at ingress." [../reference/02-graph-model.md §worked sequence](../reference/02-graph-model.md#L425-L448) covers ROUTER specifically: "the wrapped TLV's own trailer is preserved verbatim through the wrap/unwrap cycle."

But what about non-ROUTER nested TLVs that have their own trailer? Suppose a structured `USER_RECORD` (PL=1) containing inner TLVs each with `CR=1, TS=1` trailers. A bridge re-emits the outer:
- Outer trailer: stripped on ingress, fresh trailer on egress (CRC over (payload + trailer_ts)).
- Inner trailers: preserved? Recomputed? The CRC's "trailer is append-only" rule should mean preserved.
- But inner CRC was computed over inner-payload + inner-trailer-TS. The inner-trailer-TS may be intentionally older than the outer's freshly-attached one. Replay/dedup logic tying the two could go wrong.

**Fix**: add a subsection to 01-data-format.md §nested TLVs:

> When a structured TLV (PL=1) contains children with their own trailers (`opt.CR=1`, `opt.TS=1`), the outer's trailer-attach/strip cycle MUST NOT touch the children's bytes. Children's trailers are part of the outer's payload bytes from the outer's perspective. A bridge re-emitting the outer attaches a fresh outer-trailer and leaves children unchanged.

This makes the same-substrate proof obligation extend cleanly to nested-with-trailers.

### 1.8 `length=0, opt.PL=1` corner case

A structured TLV with no children: `length=0, opt.PL=1`. Is it valid? Spec doesn't explicitly cover. By analogy with `STATUS` (which uses `length=0, opt.PL=0` as the OK sentinel), an empty structured TLV could mean "an empty container of this type, no children" or could be treated as INVALID.

**Fix**: pick one. Either:
- Allow `length=0, opt.PL=1` for any structured type — useful for "an empty SETTINGS means no overrides."
- Disallow — `length=0` always means `opt.PL=0` (empty payload, no children).

Recommended: **allow** for forward-compat; receivers iterate zero children harmlessly.

### 1.9 Address-shift implicit-`expected_count` is unsafe

[../reference/03-addressing.md §loss detection](../reference/03-addressing.md#L184-L190):

> For groups without `expected_count`, the assembler treats the largest-observed-index + 1 as the implicit `N` at deadline.

A corrupted index (cosmic ray, RAM error, a malicious peer) could push `largest-observed-index` to `0xFFFE`. The assembler waits for indices 0..65534, all but a few never arrive, and the deadline expires after the assembler has held tens of MB. With no upper bound on the implicit N, this is a memory amplification vector.

**Fix**: bound the implicit `N` by `min(largest-observed-index + 1, 256)` (or similar) by default, and require the publisher to declare `expected_count` in `:settings.address_shift.expected_count` for groups beyond that bound. A subscriber that receives indices > this bound without an explicit declaration MUST emit `STATUS=ERROR(ADDRESS_SHIFT_GAP)` and discard the group.

Alternatively (and more robust): require the publisher to write a leading `:manifest` field with `expected_count` for any group of N > 1 — this also unblocks the manifest pattern from the 98-architecture-review fixlist.

### 1.10 Error-code registry has gaps

[../reference/05-protocol-tlvs.md §error code registry](../reference/05-protocol-tlvs.md#L370-L389) defines `0x00` (OK) through `0x0E` (PATH_IN_USE), with `0x0F – 0x7F` reserved.

But the spec uses error names that *aren't* in the registry:

| Used in | Code that doesn't exist | Suggested |
| ---- | ---- | ---- |
| 03-addressing.md (general invalid input) | `INVALID` | `0x0F INVALID` (general structural invalidity, distinct from `INVALID_PATH`) |
| 04-communication-flows.md §TIME §reject pre-epoch | `INVALID_PATH` is misused | `0x0F INVALID` |
| 07-host-embedding.md (peer-id collision) | `PATH_IN_USE` (semantic stretch acknowledged) | `0x10 PEER_ID_COLLISION` |
| 04 §read settings.reliability=reliable blocks | `BLOCKED` (implicit) | not strictly needed |
| §1.6 above (ROUTER parity) | `INVALID` | `0x0F INVALID` |

**Fix**: assign `0x0F INVALID` and `0x10 PEER_ID_COLLISION` (or similar) and update the cross-references. Without them, implementations will pick whatever and diverge.

### 1.11 Path canonicalization "MAY normalize NFC" is a cross-impl interop bomb

[../reference/03-addressing.md §path canonicalization](../reference/03-addressing.md#L242-L248):

> UTF-8 normalization: implementations MAY normalize path bytes to NFC at the parse boundary.

A `MAY` here is a footgun. Implementation A (Linux glibc) normalizes; Implementation B (an MCU with no ICU) doesn't. Two peers send the same human-typed path; their PATH TLV bytes diverge; dispatch table miss; no error reported (the lookup just doesn't match).

The recommendation in the doc ("application authors generally use ASCII-only path components") is true but unenforceable.

**Fix**: pick ONE. Either:
- (recommended) **Normative MUST NOT normalize** — paths are byte-equal sequences, period. Senders that want NFC normalization do it once at registration. Conformance vectors include a non-NFC path that must dispatch identically.
- Normative **MUST normalize to NFC**. Increases code-size on MCU; probably not the right call.

### 1.12 The `:` separator vs index-on-array-field is parser-tricky

A field path like `:subscribers[3].liveness.last_seen_ns` parses as field-chain `subscribers[3]` then `.liveness` then `.last_seen_ns`. Fine. But:

- `:subscribers[3]` (write a SUBSCRIBER record at slot 3) and `:subscribers[3].liveness` (write a sub-field) are syntactically different. The parser needs to handle "field with index, terminating the chain" vs "field with index, then more chain."
- `:subscribers[]` is special — append; reading returns a list. What about `:subscribers[].liveness`? Probably nonsensical (no slot to read the liveness of); should be rejected.

These edge cases aren't enumerated. Conformance vectors will need them.

**Fix**: add a §field-chain edge cases subsection enumerating which forms are valid, with examples and expected errors.

### 1.13 Cross-doc: schema can't actually describe roles

[../reference/11-vertex-roles-and-aggregation.md](../reference/11-vertex-roles-and-aggregation.md#L353-L368) lists 7 vertex roles and says the schema *hints* at which role but cannot enumerate it exhaustively. The schema discipline in [../reference/02-graph-model.md](../reference/02-graph-model.md#L334-L351) lists the 13 core writable fields but doesn't include any role-discovery field.

So a subscriber wanting to know "is this stored-value or sink-with-model?" — the canvas Mode-A vs Mode-B question — has no protocol surface to ask. Doc 11 acknowledges this and treats it as deliberate; cycle 5 below challenges whether it should be.

---

## Cycle 2 — Cross-language implementation analysis

The spec is C23-anchored but claims language-agnosticism. Each target language stresses different parts of the abstraction.

### 2.1 C23 (the reference) — what's well-shaped, what's risky

**Well-shaped:**
- 4-byte packed header maps to `[[gnu::packed]]` struct cleanly.
- Atomic refcount via `<stdatomic.h>` is canonical Boost-intrusive-ptr.
- View struct + linked-list rope is naturally pointer-based.
- `_Static_assert` makes the `TRACER_PATH(...)` macro validate at compile time.

**Risky:**
- C23 features (`_BitInt`, `<stdbit.h>`, `<stdckdint.h>`, `nullptr`, `[[nodiscard]]`) are not yet ubiquitous. ESP-IDF 5.3+ on GCC 13 supports it; arm-none-eabi-gcc 14 supports it. An older compiler (GCC 12, Clang 17) breaks the build silently or noisily depending on which feature is hit. **The build matrix needs to be explicit.**
- The reference allows `LIBTRACER_NO_ATOMIC` for Cortex-M0/M0+. But the rest of the spec assumes acq_rel ordering works. A node built `NO_ATOMIC` cannot share segments across threads — but the protocol surface offers no way to declare "I'm single-threaded" so a peer can avoid expecting concurrent fan-out. Probably fine in practice (each leaf is single-threaded by config) but worth documenting.

**Action**: pin a minimum compiler version per target tier; add a `LIBTRACER_THREAD_MODE = single | multi` macro that gates the atomic and the threading invariants.

### 2.2 C++17/20

**Well-shaped:**
- `std::span<const std::byte>` for views.
- `std::shared_ptr<segment>` works but is wrong; a custom intrusive ptr is what the spec actually wants (avoid the control-block double-allocation).
- The TLV-as-cast operation maps cleanly to `reinterpret_cast<const tlv_header_t*>(view.data())` + accessor inlines.

**Risky:**
- `std::vector<uint8_t>`-as-storage is what the v0.0 code does; v0.1 wants views, not owning containers. Need to be disciplined.
- `std::variant<TLVKinds...>` is tempting but wrong — the same-substrate insight forbids the decode step.
- `-fno-exceptions / -fno-rtti` on MCU cuts out half the C++ standard library. Need to spec exactly which subset is allowed (no `<stdexcept>`, error returns instead of throws, manual vtable for `point_i`-equivalents if any).
- The README's "no-exceptions" claim conflicts with the existing code (see 1.2). Pick.

**Action**: ship a `core/include/libtracer/cxx/` namespace with thin RAII wrappers but a C ABI underneath. Wrappers can use exceptions in hosted builds and `tl::expected<T,E>` in MCU builds.

### 2.3 Rust

**Well-shaped:**
- `Bytes` from the `bytes` crate IS the libtracer view (refcounted, sliceable, zero-copy). `BytesMut` for builders.
- `Arc<Segment>` for the segment refcount; the `bytes` crate already integrates atomic reference counting.
- `bytes::Buf` and `BufMut` give scatter-gather semantics for ropes.
- `#[repr(C, packed)]` for the wire header; `bytemuck::Pod` (or `zerocopy`) for safe casts.

**Risky:**
- The `view_t.next` linked-list rope is **un-Rusty** — a `Vec<Bytes>` or `SmallVec<[Bytes; 4]>` is more idiomatic and faster (no pointer chasing, no allocator pressure). But the C reference impl will use linked-list ropes. Cross-impl, the rope's *byte serialization* is what matters — the in-memory shape is each impl's choice. Document that the linked-list rope is one valid in-memory representation.
- The "ownership transfers at write" semantic maps to Rust's move semantics: `tracer_write(handle: &PathHandle, value: Tlv) -> Result<(), Error>` — `value` is consumed by-move. But the C API takes a borrowed `view_t *` that the dispatcher takes ownership of (refcount transfer); FFI requires care.
- The TLV-as-cast involves `unsafe { std::mem::transmute(...) }` or `bytemuck::from_bytes`. With a `#[repr(C, packed)]` header struct, this is sound, but every accessor that returns the inner length/type must carefully handle alignment — packed-struct field references are UB in Rust 1.x without going through `addr_of!` / `read_unaligned`.
- `no_std` is the MCU target; the binding crate ([../../bindings/rust/src/lib.rs](../../bindings/rust/src/lib.rs#L1-L6)) declares `#![no_std]` but is otherwise empty. Need to decide: are the Rust bindings a thin FFI over the C core, or a pure-Rust impl? Both are valid; the README says "thin wrappers" but the spec's "second implementation" gate begs for pure-Rust eventually.

**Action**: ship two crates — `libtracer-sys` (raw FFI) and `libtracer` (idiomatic wrapper). Plan a pure-Rust impl as the conformance gate (per [../reference/00-overview.md §implementation-language portability](../reference/00-overview.md#L177-L193)) for v0.2.

### 2.4 Go

**Well-shaped:**
- `[]byte` slice = view; sub-slice = sub-view (zero-copy).
- Garbage-collected segments — no explicit refcount needed for in-process use.
- `encoding/binary` for endianness.
- Goroutines + channels for the dispatcher.

**Risky:**
- The refcount abstraction *is* meaningful when bytes come from a non-GC'd source — DMA buffers, mmap'd MMIO, cgo-allocated pools. A pure-Go port would treat these as opaque and copy at the boundary, defeating zero-copy.
- Goroutine-per-subscriber is wrong at scale; need one dispatcher goroutine + bounded channels per subscriber.
- The TLV-as-cast operation maps to `*tlvHeader` over `unsafe.Pointer(&buf[0])` — works for aligned offsets only. With `LL=0` default, header is 4-byte-aligned, payload is 4-byte-aligned; should be fine.
- Go has no compile-time `TRACER_PATH(...)` macro equivalent. `go:generate` could emit static `var SENSOR_TEMP_PATH = []byte{0x06, 0x40, ...}` declarations from a string list, but it's a build-time step the user has to run.

**Action**: Go binding is post-v0.1. When done, cgo-against-C-core is fine for v1; pure-Go for v2.

### 2.5 TypeScript / JS (browser, Node, WASM)

**Well-shaped:**
- `Uint8Array` over `ArrayBuffer` for views; `ArrayBuffer` slicing is zero-copy semantically (the underlying buffer is shared).
- `DataView` for endian-aware multi-byte reads.
- WebSocket transport works for browser; TCP works for Node.
- Static path handles: a module-level `Uint8Array` constant — works.

**Risky:**
- No control over GC. The refcount abstraction is moot; segment lifetime extends as long as any view references it (via the GC root chain).
- CRC-32C in JS is software-only at ~10–50 MB/s — fine for control-plane bandwidth, painful for data-plane streaming. Browsers don't have hardware CRC instructions exposed to JS.
- "Off-loop" parsing of large TLVs needs a Web Worker; the current `transport_ws` plan ([../reference/10-module-catalog.md](../reference/10-module-catalog.md#L107)) doesn't address this.
- BigInt for u64 timestamps. `Number` loses precision past 2^53 ns, which is reachable (year 2255 from epoch). MUST use BigInt for the trailer TS.

**Action**: TS binding is post-v0.1. Document the BigInt requirement now so the test vectors catch it.

### 2.6 Python (CPython)

**Well-shaped:**
- `memoryview` over `bytes` / `bytearray` is a view.
- CPython refcounting handles segment lifetime automatically.
- `struct.unpack_from` for header parse.

**Risky:**
- Pure-Python TLV parsing is slow (microseconds per TLV). Anything throughput-sensitive needs a C extension via `cffi` / `pybind11` / PyO3. So Python is realistically a control / tooling language, not a publisher.
- GIL means dispatcher concurrency requires multiprocessing, which loses zero-copy without SHM.

**Action**: Python is for `tracer-top`, `recorder` post-processing, conformance vector generation. Not a first-class publisher language.

### 2.7 Cross-language gotchas the spec doesn't address

These show up when you try to run two implementations against each other:

1. **Endianness on big-endian targets.** Spec says LE; MIPS-BE, PPC-BE, SPARC pay per-field byte swap. Spec doesn't enumerate big-endian targets explicitly; needs a "big-endian targets MUST byte-swap on parse" sentence.
2. **Atomicity guarantee for the refcount.** Spec says acq_rel for decrement, relaxed for increment. C++ and Rust honor this; Go's `sync/atomic` doesn't expose acq_rel directly (it's seq_cst). For pure-Go subscribers, the over-strong ordering is correctness-safe but adds latency. Worth a note.
3. **Path handle ABI.** The reference C ABI defines a path handle as a pointer. Rust binding wraps `&'static [u8]`. TS uses `Uint8Array`. None of these can be passed across language boundaries without re-validation. **Document explicitly that path handles are intra-process objects; cross-process they're just bytes.**
4. **Static assert / compile-time validation.** C23 has `_Static_assert`, Rust has `const fn`, but TS has nothing equivalent. If the `TRACER_PATH(...)` validation is only enforced in C and Rust, a TS publisher can silently emit malformed PATH TLVs that C peers will reject as `INVALID_PATH`. Make the validation rules clear; ship a TypeScript helper that enforces them at runtime.

---

## Cycle 3 — Adversarial / corner cases

These are scenarios where stress, ambiguity, or malice reveals an undocumented behaviour.

### 3.1 Bridge dedup poisoning across reboots / clock resets

A peer with no RTC and no PTP boots, claims wall-clock = 0, and starts publishing. After several minutes, it gets NTP and jumps clock to current time. Subsequent writes have higher timestamps; older writes (still in bridges' recent-sets across the network) are forgotten naturally as the LRU rolls.

But a peer that crashes and reboots, claiming wall-clock = 0 again, replays origin_timestamps that are *still in some bridge's recent-set* — those duplicate-keyed legitimate post-reboot writes get silently dropped.

**Severity**: medium. Common on Cortex-M0 deployments without RTC.

**Fix**: include a `boot_id` (random UUID generated per boot) as part of the dedup key:

```
recent_set_key = (origin_peer_id, origin_boot_id, origin_timestamp)
```

`origin_boot_id` would be a new ROUTER metadata field. Adds 16 bytes per ROUTER but kills this whole class of bug.

### 3.2 Re-entrant write inside a subscriber callback

A subscriber's recv-callback writes to another path that has the original subscriber re-subscribed (transitively). If the dispatcher re-enters the callback within the same write context, you get either:
- Stack overflow (recursive dispatch).
- Out-of-order delivery (the second write is processed before the first finishes).
- Subscriber state corruption.

**Spec says**: [../reference/07-host-embedding.md §cycle handling](../reference/07-host-embedding.md#L26-L29):

> Subscriptions can introduce structural cycles only if a subscriber writes back into a vertex it transitively listens to. This is application-level; the local graph data structure does not enforce DAG-ness on subscription edges.

But this is only about cycles at the bridge layer. Local re-entry within one process isn't covered.

**Fix**: add a §re-entrant write subsection to 04-communication-flows.md normatively requiring:
- Either: per-thread re-entry counter; depth > N (recommended 16) returns `STATUS=ERROR(NESTING_TOO_DEEP)`.
- Or: dispatcher uses a work queue, not recursive calls — every `tracer_write` returns immediately and the second-level write is enqueued (loses synchronous semantics but is safer).

Recommended: **work-queue dispatch** at L4 with a documented "callback runs after all sibling subscribers are dispatched but before next write." Aligns with the spec's "no bytes copied at fan-out" — the queue holds views, not bytes.

### 3.3 CRC tampering attack on insecure transports

Spec calls CRC-32C an integrity check. Without a key, an attacker on a CAN bus or a Wi-Fi link can modify any payload byte, recompute CRC-32C, and the receiver accepts. The spec says security is post-MVP; the *language* throughout the suite uses "verify" and "validate" which connote integrity that doesn't exist.

**Fix**: add a §security-clarifications subsection to 01-data-format.md:

> CRC-32C and CRC-16-CCITT detect bit flips and transmission errors. They are NOT cryptographic integrity protection. An attacker with wire access can substitute any payload and recompute the CRC. Adversarial integrity requires a `security_*` module wrapping the transport.

Then audit the docs for "validate" / "verify" / "integrity" and replace with "frame check" where appropriate.

### 3.4 Dispatcher and subscriber thread-affinity / NUMA

[../reference/02-graph-model.md §required atomic operations](../reference/02-graph-model.md#L249-L260) covers refcount ordering. But:

- On a 4-socket Linux server, refcount on a hot segment migrates the cache line across sockets — the 100-300 ns penalty per atomic the architecture review estimates ignores cross-socket cost (which is 2-5x worse).
- The spec doesn't mention NUMA. Implicit assumption: single-socket.

**Severity**: medium. Affects only the multi-socket Linux server use case (Iceoryx2 / DDS competitor scenario).

**Fix**: add NUMA notes to [../plans/98-architecture-review.md §blocker 3 — refcount fan-out contention](../plans/98-architecture-review.md#L233-L241), promoting "sharded refcounts per socket" to a known mitigation strategy. Document that v0.1 targets single-socket; multi-socket is post-MVP.

### 3.5 Single-publisher invariant for address-shift assembly is silent

[../reference/03-addressing.md §address-shift slicing](../reference/03-addressing.md#L138-L191) says "all slices with the same `ts` belong to the same logical message." But there's an implicit assumption: only one publisher per group.

If two publishers happen to share a wall-clock timestamp (collision is rare but possible — low-resolution clocks, deliberate PTP-aligned writes), their slices get merged. The assembler emits a Frankenstein.

**Fix**: address-shift's group key should be `(origin_peer_id, ts)`, not just `ts`. Within a group, the publisher's identity is implicit because slices come from one source. Document this. Add `origin_peer_id` to the wildcard-match metadata so subscribers can distinguish groups from different publishers at the same timestamp.

### 3.6 Wildcard subscriber explosion

A subscriber registers `/**`. Every write on every path matches. Dispatch cost per write = O(wildcard subscribers) + O(concrete subscribers).

[../reference/03-addressing.md §match cost](../reference/03-addressing.md#L128-L131):

> Implementation guidance (not normative): pre-compute the matching set of wildcard subscriptions per concrete write path, cached, invalidated when subscriptions change.

But there's no cap on wildcard subscribers. A buggy or malicious subscriber registers `/**` from 100 different identities. Every write is now 100x more expensive.

**Fix**:
- Add `:settings.max_wildcard_subscribers` (default 16, per-vertex).
- ACL on `/`-rooted wildcards (only privileged subscribers can register `/**`).
- Document that `/**` is a discovery / debug pattern, not a production subscription model.

### 3.7 Refcount ABA hazard

A segment goes `refcount → 0 → destroyed → its address reused for a fresh segment with refcount = 1`. A thread holding a *stale* pointer to the old segment increments and operates on the new one. Standard ABA.

The spec doesn't call this out; the implicit rule is "never operate on segment pointers without holding a refcount." Boost intrusive_ptr discipline. But Rust's `Arc` and Go's GC handle this for free; C code MUST be disciplined.

**Fix**: add a §segment pointer discipline subsection to 02-graph-model.md or 08-views-and-ownership.md:

> A thread holding a segment pointer MUST hold a refcount on that segment for the entire lifetime of the pointer. Acquiring a segment pointer from a shared structure (a vertex map, a subscriber queue) MUST atomically increment the refcount as part of the acquisition. Implementations MUST use the canonical Boost-intrusive-ptr discipline for raw pointers, or equivalent (`Arc::clone`, atomic-CAS-on-acquire).

### 3.8 PATH TLV in `.rodata` corrupted by buggy DMA / firmware update

`.rodata` is read-only by MMU/MPU on most targets. But on Cortex-M0/M3 without MPU, a misconfigured DMA descriptor can write into flash space (with the right pre-flash write enable). The static handle's bytes change. Dispatcher's hash lookup may have cached the original hash — different bytes, different hash, dispatch miss with no error.

**Severity**: low (firmware bug, not adversarial), but the spec celebrates `.rodata` immutability and a developer trusting that wholesale could miss it.

**Fix**: an aside in [../reference/03-addressing.md §static path handles](../reference/03-addressing.md#L249) noting that hardware MUST protect `.rodata` (MPU on Cortex-M0 MPU-equipped variants; QSPI write-protect on STM32; on toolchains without MPU, the constant is in RAM and the immutability is only a convention).

### 3.9 Two routers with the same peer-id (factory misconfigure)

Two peers boot with the same `peer_id` (e.g., factory-burned SoC IDs accidentally repeat, or operators clone a device). Dedup recent-sets see the same `(peer_id, ts)` from different writes; legitimate messages get dropped.

Spec mentions this in [../reference/07-host-embedding.md §bridge identity](../reference/07-host-embedding.md#L131-L132): "Two nodes with identical peer-ids on the same network is a misconfiguration; discovery modules SHOULD emit `STATUS=ERROR(PATH_IN_USE)` (semantic stretch) on collision." But:

- "SHOULD emit" implies the network may keep operating; the dedup pollution continues until human intervention.
- Not all discovery modules are loaded; static configs don't detect collisions.

**Fix**:
- Stronger MUST: a bridge that observes two distinct peer-ids → MAC mappings (or whatever its discovery layer surfaces) MUST refuse to forward and emit a normative `STATUS=ERROR(PEER_ID_COLLISION)` (new code from §1.10).
- Document the boot_id mitigation from §3.1: even with colliding peer-ids, distinct boot_ids would let dedup distinguish.

### 3.10 Append race on `:subscribers[]`

Two writers race `write("/x:subscribers[]", sub_a)` and `write("/x:subscribers[]", sub_b)` simultaneously. Both `[]` (append) operations need a slot index assignment. Are both assigned distinct slots, or do they race on slot N?

[../reference/03-addressing.md §reading vs writing array slots](../reference/03-addressing.md#L82-L88):

> `write("/x:subscribers[]", tlv)` allocates the next free slot and places the TLV there.

"Allocates" implies atomicity inside the dispatcher, but the spec doesn't say so explicitly. Two concurrent appends could either (a) both get fresh slots (good), or (b) both get the same slot (last-write wins, one append is silently lost).

**Fix**: add normative atomicity rule:

> Append-form writes (`[]` index) MUST be serialized at the vertex's atomic-update boundary; concurrent appends MUST receive distinct slot indices. The implementation reports the assigned slot index via the write's return value (in implementation-defined form — typically by reading `:subscribers[]` post-append and matching the SUBSCRIBER's `subscriber_id` field).

### 3.11 Schema is read-only — but what when the application updates its model?

[../reference/02-graph-model.md §schema and field discipline](../reference/02-graph-model.md#L334-L351) says schema is read-only. But applications evolve: a vertex adds a field. What's the migration story?

Architecture review's blind spot 2 (schema versioning) covers this generically. But the *application-side* mechanics aren't covered: does the implementation's vertex registration API allow re-registering with a different schema? Does that invalidate cached schemas in subscribers? Spec doesn't say.

**Fix**: add §schema mutability subsection:

> A vertex's schema MUST be immutable for the vertex's lifetime. To change a schema, the application MUST unregister the vertex (causing subscribers to receive `STATUS=ERROR(NOT_FOUND)`) and re-register with the new schema. The path may be reused; subscribers re-subscribe under the new schema. This is the explicit migration boundary.

### 3.12 Backpressure across bridges

Slow subscriber on host C → TCP bridge from B → CAN sensor on A. Backpressure (queue full, segment refcount stuck high) surfaces *locally* on C. How does it propagate to B's TCP bridge, then back to A's CAN publisher?

Architecture review's blocker 5 (no flow control) covers the publisher side. The cross-bridge propagation is harder:
- Bridge B's TCP socket fills (TCP-level backpressure to A).
- A's CAN publisher sees TCP send-queue back up.
- A's CAN publisher applies QoS policy at send.

But what about the *first-hop* CAN publisher's awareness of the *terminal* C subscriber? They never communicate directly. No mechanism for C → B → A backpressure signal.

**Fix**: this is post-v0.1 (per architecture review), but document the gap explicitly so users don't expect it to work. Recommended approach for v1.x: a `STATUS=BACKPRESSURE` message that propagates origin-ward through bridges, decremental backpressure ack with bounded TTL.

### 3.13 In-flight TLVs after unsubscribe

[../reference/04-communication-flows.md §unsubscribe](../reference/04-communication-flows.md#L165-L169):

> Any in-flight TLVs already dispatched but not yet consumed by the subscriber's queue are NOT recalled. The subscriber may receive a few more TLVs after the unsubscribe call returns.

Honest. But in some impls the "in-flight" window is unbounded — a slow subscriber could receive *thousands* of late TLVs after unsubscribing. There's no upper bound on lateness.

**Fix**: add `:settings.unsubscribe_grace_window_ms` (default 100) — TLVs queued more than this many milliseconds after unsubscribe-was-observed-by-dispatcher MUST be dropped, even if the subscriber's queue still has space. Bounded leak.

---

## Cycle 4 — Architectural rationale (and where each load-bearing choice leaks)

For each of the load-bearing claims and major architectural decisions, why is it the way it is, and where does it leak?

### 4.1 Why TLV (not JSON / Protobuf / Cap'n Proto / FlatBuffers)?

**Reason**: branchless 4-byte parse, fixed-width length, no schema dependency, the same bytes are graph node and wire frame. CRC at known position. Cap'n Proto / FlatBuffers solve schema-on-the-wire; libtracer deliberately doesn't.

**Leak**: zero schema discipline. The user-range `0x80–0xFF` is a wild west. Cross-project type-code coordination is on the user. Two unrelated projects WILL collide at type code `0x80`. Mitigation in [../reference/05-protocol-tlvs.md §user range](../reference/05-protocol-tlvs.md#L639-L641) ("magic prefix") is voluntary.

**Tightening**: register an IANA-style central type-code registry (could just be a markdown table in `docs/spec/registry.md`). Projects PR their use; collisions surface at PR time.

### 4.2 Why "API is read/write only"?

**Reason**: smallest possible primitive surface; subscribe-as-field-write keeps connection state out of the protocol; testability skyrockets.

**Leak**:
- Schema discoverability becomes critical (you can't do anything without knowing fields). Tooling (`tracer-top`, web GUI) is post-MVP.
- The `:` separator + field-chain syntax is more complex than connect/subscribe. The user is paying complexity at the addressing layer instead of at the API layer.
- Atomic multi-field writes via "write a SETTINGS to the parent path" is a real cognitive jump for developers used to RPCs.

**Tightening**: ship `tracer-top` early in the roadmap (it's currently week 8); make it a week-2 milestone so developers have an introspection tool while the API is still being finalized.

### 4.3 Why "no fragmentation in the wire format"?

**Reason**: each slice independently routable; transport choice is invisible to subscribers; no reassembly state to corrupt.

**Leak**: dispatch overhead is N times for N slices. Architecture review's manifest-pattern fix is necessary; without it, the streaming claims are aspirational.

**Tightening**: reserve `0x0E MANIFEST` type code in [../reference/05-protocol-tlvs.md](../reference/05-protocol-tlvs.md#L621-L629) NOW (already a candidate; promote to v0.1 to avoid a registry change later). Define `MANIFEST { NAME "expected_count" VALUE u32, NAME "publisher" PATH, NAME "ts" TIME }`.

### 4.4 Why "bridges in core"?

**Reason**: cross-bus deployments are first-class. A CAN+IP+SHM unified address space is the differentiator.

**Leak**:
- Recent-set sizing is non-trivial; bursts cause LRU eviction and storm potential.
- ROUTER overhead per crossing — every bridge hop adds 30–60 bytes of metadata.
- Even a P0 in-process build links the bridge module (it's `required`). Wasted flash on a leaf node that has no transport.

**Tightening**:
- Move bridge to `required-when-≥2-transports`. P0 and P1 builds don't link it. The architecture review's profile table already implies this; just make it explicit in [../reference/10-module-catalog.md](../reference/10-module-catalog.md#L92).
- Cap ROUTER metadata: a frozen, ordered set of fields with no extensibility, so the per-hop overhead is bounded.

### 4.5 Why "the graph imposes no shape"?

**Reason**: full flexibility — 1 byte boolean to 10 GB camera frame, MMIO register to function-on-read, all under one API.

**Leak**: no schema registry, no type-safety, application carries the burden. The 7-role taxonomy in [../reference/11-vertex-roles-and-aggregation.md](../reference/11-vertex-roles-and-aggregation.md) is doc-only — nothing in the protocol surfaces a vertex's role to a peer.

**Tightening**: optional `:role` field (an enum: stored / stream / sink-with-model / computed / proxy / aggregate / live-mmio). NOT mandatory; if absent, peers assume stored-value (the safest default). Documented in 02-graph-model.md as a peer-discoverable hint.

### 4.6 Why "paths encoded once, used many times"?

**Reason**: ISR-safe writes; no `snprintf` / `malloc` on hot path; 16 KB Cortex-M0 budget viable.

**Leak**:
- PATH TLV bytes are large for short paths (22 bytes for `/sensor/temp`). For a 1-byte payload, the path is 22x the payload. Acceptable for control-plane writes, expensive for high-rate small payloads.
- All hot-path performance claims rest on this discipline. If the reference impl regresses, the system's "fast" claim collapses.

**Tightening**: a *very short path* (< 4 bytes total segment bytes) might warrant a different encoding (a future `0x0E SHORT_PATH`?) — but that's a v1.x optimization. For v0.1, document the 22-byte minimum and design around it.

### 4.7 Why no per-frame version bit?

**Reason**: forces design rigour; one-shot wire format commitment.

**Leak**: doors for evolution are limited to (a) type-code registry `0x0E–0x7F`, (b) discovery-layer name change for incompat. (b) is heavyweight.

**Tightening**: clarify the dimensions of evolution that the registry can actually accommodate:
- New type codes — yes, in-scope.
- New `opt` bits — no, reserved bits are forever frozen (per §1.4).
- New error codes — yes, `0x0F–0x7F` and `0x80–0xFF`.
- New ROUTER metadata fields — yes (NAME-tagged, parsers skip unknown).
- New mandatory header semantics — no, requires v2.

Make this an explicit table in 01-data-format.md §forward extension path.

### 4.8 Why "trailer is append-only at egress"?

**Reason**: payload bytes invariant under all transitions (record, replay, bridge, dedup). Same-substrate proof.

**Leak**: trailer's CRC must cover (payload + trailer_ts) which couples those two trailer fields' integrity. Streaming validation is doable but must be implemented carefully — a receiver that validates CRC over payload only (forgetting trailer_ts) accepts corrupted timestamps with valid payload CRC.

**Tightening**: explicitly call out the CRC coverage in conformance vectors. The vector for "TLV with TS+CR" must include a case where trailer_ts is corrupted while CRC is recomputed only over payload → MUST fail.

### 4.9 Why "every host is a router"?

**Reason**: simplest scaling story; no architectural distinction between leaf and bridge.

**Leak**: a leaf node still links the bridge module. Flash cost is paid even when unused.

**Tightening**: see 4.4 above — move bridge to optional for P0/P1.

### 4.10 Why six-layer model?

**Reason**: clean separation; conformance per-layer; concepts don't leak across layers.

**Leak**: more layers = more conceptual overhead. Newcomers struggle with L0 vs L1 distinctions. The reading-order paths help, but not enough.

**Tightening**: a single-page "L0..L5 cheat sheet" at the top of [../reference/00-overview.md](../reference/00-overview.md) showing one TLV's byte path through all six layers, with each layer's contribution highlighted. The end-to-end DMA→ADC trace in [../reference/08-views-and-ownership.md](../reference/08-views-and-ownership.md#L473-L582) is great but buried.

### 4.11 Why retire LIST `0x05`?

**Reason**: type code IS purpose; no semantic-less generic structured container; forces meaningful naming.

**Leak**: user-range structured records use `0x80–0xFF` with `PL=1`, no central registry. Same problem as 4.1.

**Tightening**: same as 4.1 — central registry.

### 4.12 Why selectable u16 / u32 length?

**Reason**: 4-byte header for typical TLVs (≤ 64 KiB) saves 2 bytes/frame; u32 available for large ones; u64 deliberately absent to force address-shift discipline.

**Leak**: implementations that always use LL=1 (because their toolchain is lazy or the developer didn't know) waste 2 bytes per frame. Spec says SHOULD use smaller variant by default; not always honored.

**Tightening**: conformance vectors include a "minimum TLV byte count" test that rejects unnecessarily-large encodings. Only feasible if there's a normative MUST: "senders MUST use the smallest LL/CW/TF that fits." Currently SHOULD; consider promoting to MUST.

---

## Cycle 5 — Questions you haven't asked but should

Each of these is something a senior implementer or operator would raise that the current docs don't answer.

### 5.1 What is the conformance test harness?

`tests/conformance/` exists with a README; no vectors. The spec ([../spec/v1.md §4](../spec/v1.md#L86-L93)) gates conformance on test vectors. Who maintains the vectors? What format (golden bytes? expected-output trees?)? When does the first vector ship?

**Action**: schedule "vectors v0" as a milestone before week 4 of the 8-week plan; without them, "conformance" is undefined. Recommended format: per-test directory with `input.bin`, `expected.json` (parsed structure), `description.md`.

### 5.2 What is the upgrade path from v0.0 (existing code) to v0.1?

Current `tlv.h` is v0.0-shaped. The README says "v0.0 has no users; bump version byte and don't carry the cruft." That's nominally OK, but:
- The integration packages (PlatformIO, ESPHome, Arduino) reference v0.0-style? — they reference `library.json` etc. but the actual Arduino headers will be installed into user projects. If anyone has shipped pre-release builds, the migration is non-trivial.
- The conformance commitment pinning ("v1 is immutable once finalized — corrections require v2") in [../spec/v1.md](../spec/v1.md#L96-L98) means once you ship vectors, you can't change anything semantic.

**Action**: explicit "v0.0 → v0.1 transition note" in the changelog, naming the wire breaks (CRC, length, header position, LIST retirement, opt-bit redefinition, NAME-no-NUL). Clear that no v0.0 peer can interoperate with v0.1.

### 5.3 How do you debug protocol-level bugs (not application bugs)?

The architecture review's blind spot 5 (observability) covers this. But beyond metrics, what about:
- Per-write tracing (which subscribers received it, when, with what latency)?
- Bridge dedup hit/miss counters per origin_peer_id?
- Recent-set utilization?
- Wildcard match cost histograms?

Without these, debugging "why was my message dropped?" across 4 bridges and 3 transports is impossible.

**Action**: define a `:_diag` namespace per node (note the `_` prefix to avoid collision with user paths) with normative paths:
- `/_diag/dispatcher:writes_total`
- `/_diag/dispatcher:writes_dropped[{drop_reason}]`
- `/_diag/bridge[N]:dedup_hits_total`
- `/_diag/bridge[N]:dedup_evictions_total`
- `/_diag/transport[N]:bytes_in_total`, `:bytes_out_total`

Promote architecture review's P1 #9 ("standard `:metrics.*` namespace") to v0.1.

### 5.4 What is the threading model contract?

Big gap. Let's enumerate what the spec doesn't say:
- In which thread is a subscriber's callback invoked? (Publisher's? Dispatcher's? Per-subscriber thread?)
- Can a callback call `tracer_write` recursively?
- Can a callback block? If yes, what's the back-pressure on other subscribers?
- Can two callbacks for the same subscriber run concurrently from two different writes?
- What's the memory model between publisher's pre-write writes and subscriber's post-callback reads?

Without these, every implementation will pick differently and applications will break across them.

**Action**: add a normative §threading model to 04-communication-flows.md:

> Subscriber callbacks are invoked on an implementation-defined thread; the callback MAY be invoked from the publisher's thread, the dispatcher's thread, or a per-subscriber worker thread. Implementations MUST guarantee:
> 1. Callbacks for the same subscriber are serialized (no two concurrent invocations).
> 2. Callbacks MAY call `tracer_write`, `tracer_read`, `tracer_await`, but MUST NOT block on completion of fan-out from the original write (re-entrant write rules of §3.2).
> 3. The view's bytes remain valid for the lifetime of the callback invocation; after return, the implementation may release.

### 5.5 What's the persistence + replay story?

Recorder is post-MVP. But recording semantics need answering NOW so the wire format doesn't preclude them:
- Does the recorder preserve order? Best-effort wall-clock order, or delivery-arrival order, or per-source FIFO?
- Bridged TLVs may be recorded twice (origin's recorder + downstream bridge's recorder). Dedup on replay?
- Sink-with-model vertices: replay rebuilds state by replaying writes; if any write is missing, model diverges. Compensating mechanism?

**Action**: a one-page §recording-and-replay subsection in 04-communication-flows.md outlining the v0.1 baseline (per-source FIFO; replay is a write-stream as if the original publisher had emitted; dedup-by-(origin, ts) preserves cycle safety). Doesn't have to be implemented now, but the contract gates the wire format.

### 5.6 What about big-endian targets?

The spec says LE everywhere. Big-endian targets (mips-be, sparc, ppc-be) need byte swap on every multi-byte field. Spec doesn't enumerate them or call out the cost. Probably most users never hit this — but if anyone tries to build on a Cisco/IBM/legacy embedded BE target, they'll be surprised.

**Action**: a sentence in 01-data-format.md §endianness:

> Implementations on big-endian hosts MUST byte-swap all multi-byte fields on parse and emit. The cost is 4–8 cycles per field on modern CPUs; bandwidth is the same. Big-endian targets are explicitly supported but not v0.1-CI-tested.

### 5.7 Resource limits in the dispatcher

What's the hard maximum for:
- Vertex count per node?
- Subscriber count per vertex?
- Wildcard subscriber count per topic?
- Recent-set entries?
- Pending writes in the dispatcher queue?
- Pending await waiters per vertex?

Spec mentions a few (32 nesting, ≤ few dozen wildcards "guidance"). Without hard caps, resource exhaustion attacks / accidents are unbounded.

**Action**: a normative §resource-limits subsection in 02-graph-model.md or 04-communication-flows.md:

| Resource | Default cap | Configurable |
| ---- | ---- | ---- |
| Vertices per node | 1024 | yes |
| Subscribers per vertex | 64 | yes |
| Wildcard subscribers per topic | 16 | yes |
| Recent-set entries per bridge | 8192 | yes |
| Pending await waiters per vertex | 8 | yes |
| Subscriber queue depth | 32 | yes |

Implementations may exceed these; spec's job is to set defaults so behaviour converges.

### 5.8 What does "node init" mean for discovery-driven peers?

[../reference/03-addressing.md §init-time registration](../reference/03-addressing.md#L329-L356) implies path handles are registered during `tracer_init`. But peer-mounted paths `/peer/{peer_id}/...` only exist once discovery has found the peer, which is after init.

So those handles are *dynamic* — they come and go with peers. Spec doesn't say:
- Is the handle invalidated when the peer disappears?
- Does `tracer_write(stale_handle, value)` return `ERROR=NOT_FOUND`?
- Can the same peer-id reconnect and have the handle become valid again?

**Action**: dynamic handle lifecycle subsection in [../reference/03-addressing.md](../reference/03-addressing.md):

> A handle registered via `tracer_path_register` for an interpolated peer-mounted path remains valid as long as the registration persists, regardless of whether the peer is currently reachable. Writes through a handle whose target is unreachable return `STATUS=ERROR(TRANSPORT_DOWN)`. The handle's bytes never change; only the dispatcher's resolution outcome does.

### 5.9 Clock-skew dedup edge cases

NTP correction jumps the clock. Republished writes have older timestamps that re-collide with recent-set entries → false-positive drops. Architecture review names clock skew (blind spot 8) but doesn't cover this specific pattern.

**Action**: combined with §3.1 — `boot_id` in the dedup key fixes both reboot-clock-zero and post-NTP-jump cases. Single mitigation, two bugs killed.

### 5.10 Two markets in one product (R8 from vision)

You named this risk: RC car (microcontroller, single bus, no security, fixed config) vs HPC GPU sampling (multi-socket, high-rate, multi-tenant, NUMA, security mandatory). Same protocol surface, opposite operating points.

The v0.1 scope is "embedded + LAN" per [../plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md#L13-L17). HPC is post-MVP. But the load-bearing claims (zero-copy, single API, address-shift) imply you're going for both.

The honest answer is:
- v0.1 is a control plane in HPC contexts (good).
- v0.1 is a data plane in embedded contexts (good).
- Don't promise both as the same data plane.

**Action**: a "scope clarity" sentence at the top of [../reference/00-overview.md](../reference/00-overview.md):

> **v0.1 scope.** libtracer v0.1 is the embedded-and-LAN data plane and the HPC control plane. It is not the HPC data plane; that role belongs to RDMA / NCCL / GPUDirect, which libtracer negotiates as a control plane. See [00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md) for the full scope decision.

This already exists piecemeal; promote it to the front of every reader's mind.

### 5.11 What's the v0.1 minimum security posture?

"Decentralized graph spanning CAN+IP under one address space" without security is an operator nightmare. Architecture review names this (blind spot 1). But the integration packages already point at PlatformIO / ESPHome / Arduino — production-deployment-shaped artifacts — without a security story.

**Action**: even if `security_*` modules are post-MVP, ship a normative `security_disabled = true` flag at node init that requires explicit acknowledgement. A node configured for `transport_tcp` without `security_tls` MUST log a startup warning. ESPHome-shipped builds default to `security_required = true`, refusing to start without a security module.

This is a no-code change in v0.1 (it's policy), but it sets the expectation that production deployments need security and v0.1 ships a "this is unsafe" warning.

### 5.12 Module ABI evolution

The module ABI is "implementation concern" per [../reference/00-overview.md](../reference/00-overview.md#L172). But the C reference impl IS an implementation, and once it ships, third-party transports / discovery / executors will start linking against its ABI. ABI breaks become hard-incompat for downstream.

**Action**: declare the C reference ABI semver-stable from week 4 onward. Increment a `tracer_abi_version` symbol on every break. Don't ship an unversioned ABI.

### 5.13 What about the integration packages?

`integrations/platformio/`, `integrations/esphome/`, `integrations/arduino/` are referenced in the README. Each has its own conventions about library structure, header layout, exception support, allocator. The spec doesn't speak to integration packages at all.

**Action**: each integration package needs a one-page "what subset of libtracer does this expose, with what defaults?" — e.g., ESPHome ships P1 default with `transport_wifi` + `discovery_mdns` + warning if no `security_tls`. Arduino ships P0 with `transport_serial`. PlatformIO has lib-deps for each profile.

### 5.14 What does "frozen for v0.1" actually mean?

[../reference/README.md §promotion rule](../reference/README.md#L94-L101) says a section is frozen when (1) implemented, (2) second-implementer review, (3) conformance suite covers it. The architecture review's P0 fixlist has 6 items; some unresolved. What's the gating?

**Action**: turn the P0 fixlist into the literal freeze gate. A reference doc section moves from draft→frozen when its corresponding P0 items are all done AND the conformance vectors exist AND a second implementation passes them. Make this explicit in the README.

### 5.15 Cross-implementation testing

Even with conformance vectors, two implementations need to actually talk to each other in CI. The reference C impl + a Rust impl + a TS impl each running against each other on every PR. That's a lot of CI matrix.

**Action**: a `tests/interop/` directory with docker-compose harness running a small fleet (1 ESP32 simulator, 1 Linux router, 1 browser-WS-client) sending a defined set of TLVs to each other. Pass = bytes received match expected. Run on every PR.

This goes alongside `tests/conformance/` (offline byte-vector tests).

### 5.16 What about graceful shutdown?

A node shutting down: bridges should emit `STATUS=TRANSPORT_DOWN` to subscribers; subscribers should release segments; the dispatcher should drain. Spec is silent on the shutdown sequence. Implementations will differ.

**Action**: a normative §shutdown sequence in 04-communication-flows.md:
1. Application calls `tracer_shutdown()`.
2. All bridges emit `STATUS=ERROR(TRANSPORT_DOWN)` to their subscribers.
3. New writes return `ERROR=TRANSPORT_DOWN`.
4. Pending writes drain (bounded by `:settings.shutdown_grace_ms`, default 1000).
5. Subscriber callbacks return; segments release.
6. Module unload in reverse-init order.

### 5.17 What's the "you should not use libtracer if..." section?

Every protocol has an honest "use this when, don't use this when" guide. libtracer doesn't have one yet. Without it, users will misuse it.

**Action**: a §when-not-to-use libtracer subsection in 00-overview.md:

> Don't use libtracer if you need:
> - Cluster consensus or distributed transactions — out of scope.
> - Sub-µs guaranteed latency on multi-core Linux — refcount contention.
> - Production-grade security in v0.1 — `security_*` modules are post-MVP.
> - 22-policy DDS QoS — libtracer has 5.
> - HPC data plane bandwidth >10 GB/s — control plane only.
> - ROS / DDS bridge — not v0.1.
> - Schema evolution beyond add-only — schemas are immutable per vertex lifetime.

---

## Consolidated findings — prioritized

The findings above are organized by cycle. Here's the same set ordered by what blocks v0.1 freeze.

### P0 — block v0.1 freeze

1. **Code-vs-spec rewrite of `core/include/libtracer/*`** (§1.2). The reference impl is v0.0-shaped on every dimension. Without the rewrite, no conformance is possible.
2. **PATH TLV `opt=0x50` literal bug** (§1.1). Fix every byte literal across docs 03 / 05 / 06 — PATH outer is `06 40 12 00`, not `06 50`.
3. **Conformance vector format and first vectors** (§5.1). Without vectors, "conformance" is undefined.
4. **Error code registry gaps** (§1.10). Assign `0x0F INVALID`, `0x10 PEER_ID_COLLISION`. Update all references.
5. **Path canonicalization NFC ambiguity** (§1.11). MUST NOT normalize; senders responsible.
6. **CRC tampering / security clarification** (§3.3). Explicit "this is not adversarial integrity" text in 01-data-format.md.
7. **`tracer.hpp` duplicate `name_t` won't compile** (§1.3). Rename.
8. **Threading model contract** (§5.4). Currently undefined; will diverge across impls.

### P1 — should land in v0.1

9. **`boot_id` in ROUTER metadata** (§3.1, §5.9). Kills reboot-clock-zero and post-NTP-jump dedup bugs in one stroke.
10. **Manifest type code `0x0E` reserved** (§4.3). Address-shift dispatch amortization needs it; reserving now avoids registry churn.
11. **Bridge `required-when-≥2-transports`** (§4.4, §4.9). Saves flash on P0/P1 leaves.
12. **Append race atomicity for `:subscribers[]`** (§3.10). Currently undefined.
13. **`/_diag` namespace for observability** (§5.3). Enables debugging.
14. **Resource-limits defaults** (§5.7). Each implementation needs the same defaults to converge.
15. **Empty structured TLV (`length=0, opt.PL=1`)** (§1.8). Pick one.
16. **Wildcard subscriber cap** (§3.6). `:settings.max_wildcard_subscribers`.
17. **Address-shift implicit-N bound** (§1.9). Or require manifest.
18. **Address-shift group key includes `origin_peer_id`** (§3.5).
19. **Re-entrant write rules** (§3.2). Work-queue or depth-cap.
20. **Schema mutability rule** (§3.11). Re-register with new schema; old subscribers see NOT_FOUND.
21. **Reserved-bit forever rule** (§1.4). Explicit text.
22. **Wildcard EBNF gap** (§1.5). Subscriber-path non-terminal.
23. **ROUTER child parity rule** (§1.6). Reject odd-parity ROUTER.
24. **Nested-trailer behaviour** (§1.7). Outer attach/strip leaves children's trailers alone.
25. **Big-endian explicit support** (§5.6).
26. **`:role` discovery field (optional)** (§4.5).
27. **In-flight-after-unsubscribe grace window** (§3.13).

### P2 — post-v0.1 with reference impl

28. **Refcount sharding for NUMA / multi-socket** (§3.4).
29. **Backpressure cross-bridge propagation** (§3.12).
30. **Central type-code registry** (§4.1, §4.11).
31. **Cross-impl interop CI matrix** (§5.15).
32. **Graceful shutdown sequence** (§5.16).
33. **Module ABI semver** (§5.12).
34. **Integration package profile docs** (§5.13).
35. **Persistence / replay contract** (§5.5).

### P3 — explicit non-goals; document as such

36. **Cluster consensus / CRDTs.**
37. **Cross-vertex atomic transactions.**
38. **HPC data plane (>10 GB/s).**
39. **ROS / DDS bridge.**
40. **Schema evolution beyond re-registration.**
41. **22-policy DDS-style QoS.**

---

## Critical files to modify

The files most affected by the proposed changes, with the rough nature of each change.

| File | Changes |
| ---- | ---- |
| [../reference/01-data-format.md](../reference/01-data-format.md) | Reserved-bit forever rule (§1.4); CRC-coverage forensics in conformance (§4.8); big-endian sentence (§5.6); security clarification (§3.3); empty-structured-TLV rule (§1.8); nested-trailer behaviour (§1.7); MANIFEST `0x0E` reservation (§4.3) |
| [../reference/02-graph-model.md](../reference/02-graph-model.md) | Schema mutability rule (§3.11); segment pointer discipline (§3.7); resource limits (§5.7); optional `:role` field (§4.5) |
| [../reference/03-addressing.md](../reference/03-addressing.md) | PATH literal `0x50→0x40` (§1.1); subscriber-path EBNF (§1.5); NFC normative (§1.11); field-chain edge cases (§1.12); address-shift expected_count bound + group key (§1.9, §3.5); dynamic handle lifecycle (§5.8) |
| [../reference/04-communication-flows.md](../reference/04-communication-flows.md) | Threading model normative (§5.4); re-entrant write rules (§3.2); append race atomicity (§3.10); unsubscribe grace window (§3.13); recording-and-replay contract (§5.5); shutdown sequence (§5.16); `/_diag` namespace (§5.3) |
| [../reference/05-protocol-tlvs.md](../reference/05-protocol-tlvs.md) | PATH literal `0x50→0x40` (§1.1); error code registry assignments (§1.10); ROUTER child parity (§1.6); MANIFEST type code (§4.3); ROUTER `boot_id` field (§3.1); wildcard cap setting (§3.6) |
| [../reference/06-user-data-packing.md](../reference/06-user-data-packing.md) | PATH literal `0x50→0x40` in MCU recipes (§1.1) |
| [../reference/07-host-embedding.md](../reference/07-host-embedding.md) | Peer-ID collision normative MUST (§3.9); `boot_id` recent-set key (§3.1) |
| [../reference/10-module-catalog.md](../reference/10-module-catalog.md) | `bridge` required-when-≥2-transports (§4.4) |
| [../spec/v1.md](../spec/v1.md) | First conformance vectors path (§5.1); v0.0→v0.1 migration note (§5.2) |
| [../../README.md](../../README.md) | Reconcile "no-RTTI no-exceptions" with code (§1.2); honest "when not to use" (§5.17); P0/P1 profile defaults per integration package (§5.13) |
| [../../core/include/libtracer/tlv.h](../../core/include/libtracer/tlv.h) | **Full rewrite** to v0.1 wire format (§1.2) |
| [../../core/include/libtracer/tracer.hpp](../../core/include/libtracer/tracer.hpp) | **Delete or rewrite**; remove connect/disconnect; deduplicate `name_t` (§1.2, §1.3) |
| [../../core/include/libtracer/tlv_vector.hpp](../../core/include/libtracer/tlv_vector.hpp) | **Delete**; replace with view-based machinery (§1.2) |
| [../../core/include/libtracer/tlv_string.hpp](../../core/include/libtracer/tlv_string.hpp) | **Delete**; NAME has no NUL (§1.2) |
| [../../core/include/libtracer/serdes.h](../../core/include/libtracer/serdes.h) | **Delete**; TLV-as-cast replaces serialize/deserialize (§1.2) |
| [../../bindings/rust/src/lib.rs](../../bindings/rust/src/lib.rs) | Empty stub; plan FFI vs pure-Rust (§2.3) |
| [../../bindings/typescript/src/index.ts](../../bindings/typescript/src/index.ts) | Empty stub; plan WS / WASM positioning (§2.5) |

---

## Recommended fix sequence

The order matters because some changes unblock others.

1. **Spec micro-fixes** (1 day): §1.1, §1.4, §1.5, §1.6, §1.7, §1.8, §1.10, §1.11, §1.12. Pure docs work, no impl impact, makes the spec internally consistent.
2. **Spec normative additions** (1 week): §3.10, §3.11, §3.13, §4.3 (MANIFEST), §4.5 (`:role`), §5.4 (threading), §5.7 (resource limits), §5.16 (shutdown), §5.17 (when-not-to-use). Adds ~200 lines across files but no architectural change.
3. **`boot_id` design** (2-3 days): §3.1 + §5.9. Touches ROUTER schema, recent-set key, transport_label optionally. Worth its own ADR.
4. **Code rewrite scaffolding** (1 week): produce `tlv.h`, `view.h`, `path_handle.h` per §1.2 fix proposal. Compiles. Doesn't do anything yet. Replaces the v0.0 stubs.
5. **First conformance vectors** (1-2 weeks): §5.1. Must match the rewritten code. Pinned to spec literally.
6. **Reference impl matches vectors** (the existing 8-week roadmap, week 1-4 milestones). The roadmap is sound; just gated on the prior steps.
7. **Second-implementer review** (after step 6): per [../reference/README.md §promotion rule](../reference/README.md#L94-L101). Could be an external Rust impl, or a careful re-read by someone who hasn't seen the C code.
8. **Promote to "frozen for v0.1"**: only when steps 1–7 are done.

---

## Verification plan

Once the fixes are applied:

1. **Spec consistency check**:
   - Grep for `0x50` in `docs/reference/` — every occurrence is either in 01-data-format.md (the worked example with CRC) or has been changed to `0x40`. No PATH-without-CRC literal contains `0x50`.
   - Grep for `LIST` in `docs/reference/` — only appears as "retired" or "0x05 RETIRED."
   - Grep for `connect`, `disconnect` — only in 04-communication-flows.md as "no `connect()` primitive" prose.
   - Every error code in any normative MUST/SHOULD has a registry entry in 05-protocol-tlvs.md.

2. **Code consistency check**:
   - `cmake -B build && cmake --build build` succeeds with `-Wall -Werror -fno-exceptions -fno-rtti`.
   - `tlv.h` defines a 4-byte header; `length` is u16 by default, u32 when LL=1; no in-header CRC.
   - `tracer.h` exposes only `tracer_read/write/await(handle, view)` and registration / lifecycle helpers.
   - No `connect` or `disconnect` in any header.
   - `LIST = 0x05` is **not** defined anywhere.

3. **Conformance vector smoke test**:
   - Encode `/sensor/temp` PATH TLV in C, in Rust (when binding exists), in TS (when binding exists). Compare to `tests/conformance/vectors/v1/path/sensor_temp.bin`. Byte-equal.
   - Encode an empty STATUS=OK. Should be exactly `09 00 00 00`.
   - Encode a 5-byte VALUE with no trailer. Should be `01 00 05 00 AA BB CC DD EE`.
   - Validate CRC over a TLV with TS+CR; corrupting `trailer_ts` MUST cause CRC_FAIL.
   - Parse a deeply-nested (32 levels) structured TLV — succeeds. 33 levels — `NESTING_TOO_DEEP`.
   - Parse a malformed `opt` byte (reserved bit set) — `INVALID`.

4. **Cross-impl interop** (when both C and Rust impls exist):
   - C publisher writes `/sensor/temp` over UNIX socket; Rust subscriber reads — bytes match.
   - Reverse direction.
   - Path canonicalization: send `/sensor/temp` and (Latin-NFC-equivalent) `/sensÖr/temp` — they MUST resolve to *different* vertices (no NFC normalization per §1.11).

5. **Adversarial smoke**:
   - Send 1000 ROUTER-wrapped TLVs in a tight loop with the same `(origin_peer_id, ts, boot_id)` — bridge dedups all but 1.
   - Reboot a publisher with clock = 0 — new boot_id distinguishes from pre-reboot writes; no false dedup.
   - Send ROUTER with odd-parity children — `STATUS=INVALID`.

6. **Architecture review checkbox**: walk through [../plans/98-architecture-review.md §risk-prioritized fixlist](../plans/98-architecture-review.md#L431-L464). Every P0 item is now either resolved (path handles ✓ already; manifest ✓ via this plan; latency contract ✓ via §5.4 + this plan; lwIP/DMA OPEN questions ✓ via §3 of 09-memory-substrate.md; subscriber overflow ✓ via §3.6+§5.7; clock-skew warning ✓ via §3.1+§3.3) or explicitly deferred to P1 with rationale.

If all six pass, v0.1 is genuinely freezable.

---

## What this plan is NOT

- Not a re-design proposal. The architecture is sound. The fixes above are additive, not architectural.
- Not a marketing pitch.
- Not a benchmark report.
- Not a vote on whether libtracer should be built. Your vision doc already answered "yes, on the embedded + LAN scope."
- Not a replacement for [../plans/98-architecture-review.md](../plans/98-architecture-review.md). That review covers performance / scaling concerns; this plan covers correctness / consistency / portability concerns. Both are needed.

The two reviews together (architecture-review for "does it scale?" + this plan for "is it consistent and correct?") form a complete v0.1 freeze gate.
