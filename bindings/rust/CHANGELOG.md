# Changelog — libtracer (Rust binding)

All notable changes to the **public API** of the `libtracer` Rust crate are
recorded here, per [CONTRIBUTING](../../.github/CONTRIBUTING.md) / [CLAUDE.md](../../CLAUDE.md).
The crate is pre-1.0; the first cut release is `[0.3.0]`, below.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed

- **`encode` no longer mints a `PATH_REF` frame this crate's own `decode` rejects (#1004).** The
  grammar has exactly one per-type structural rule — a `PATH_REF` body is a fixed-stride 8-byte
  record array, so `opt.PL` and `opt.LL` are both forbidden and the length is a bounded multiple
  of 8 (RFC-0024 §4.2/§4.3) — and `parse_one` has always enforced it. The generic `encode` did
  not: it serialized any `Tlv` verbatim, so a `PATH_REF` built with `opt.pl` even took the
  children branch and emitted per-child TLV framing. All four ill-formed shapes produced bytes
  this crate answers with `Error::FrameInvalid`. The guarded `path_ref` builder satisfies the
  rule by construction, which left a `Tlv` STRUCT LITERAL as the door — `Tlv`'s fields are all
  public, so that is the ordinary way to build one here. The four clauses now live in one
  private predicate that `parse_one` and `encode` share, rather than the encoder gaining a copy.

  The C++ core closed the same asymmetry in #886 and this crate did not, so the three cores
  diverged on one input tree; that divergence is now closed on this side. No wire change, and no
  well-formed tree encodes differently.

  **API note — the failure mode is a new `encode` postcondition.** `encode` returns a `Vec<u8>`
  and has no error channel, so refusal is spelled **emits nothing**: an ill-formed `PATH_REF`
  anywhere in the tree makes the whole call return an EMPTY `Vec`, and a refused TLV refuses its
  ancestors rather than being dropped into a frame that DOES decode, one component short. Empty
  is unambiguous — an accepted TLV always carries at least its 4-byte header, so no well-formed
  tree encodes to nothing. This matches the C++ core's `wire::encode` exactly.

### Added

- **`BuildError::InvalidPathRef`** — a new variant, so `encode_fwd_bytes` can surface the
  refusal above instead of answering `Ok` over an empty `Vec`. `FwdRequest::payload` is a
  caller-supplied `Tlv` embedded verbatim, so an ill-formed `PATH_REF` reaches that
  `Result`-returning wrapper; a success carrying a frame that is silently nothing is the worst
  of the two answers. **Breaking for an exhaustive `match` on `BuildError`** (the enum is not
  `#[non_exhaustive]`); every existing variant keeps its meaning and no existing error changes.
  `encode` itself is unaffected — it still has no error channel (#1004).

- **`fwd/fwd-reply-error-after-description` conformance vector**, shared with the C++ and
  TypeScript cores and pinned here by `fwd_reply_error_after_description`. No behaviour change to
  this crate: `reply_error_tlv` already scanned the STATUS's children for the first `ERROR`, which
  is the ruled rule. What changed is that the rule is now **gated** rather than coincidental — the
  TypeScript binding required the ERROR at `children[0]` and read this frame as code `0`, so the
  two cores disagreed on which of a peer's errors were diagnosable at all (#878). Ablating this
  crate's reader to the positional rule reddens the new test with the same `0 != 32`, so the gate
  catches the drift whichever core drifts.

  The ruling, recorded on `reply_error_tlv` and in `tests/conformance/HARNESS.md`: a reply's ERROR
  is the **first `ERROR` child of the STATUS, at whatever position**. reference/05 §`0x09` pins no
  order over a STATUS's children; RFC-0002 §C pins position only one level down, inside the ERROR.
  Emitters are unaffected and still write the canonical order.

### Fixed

- **`structured::spec` emitted a SPEC no terminus accepts.** Both field values — `type` and
  `name` — were wrapped in a `VALUE` (`0x01`) node where the wire form is a `NAME` (`0x02`).
  The terminus matches each `(NAME key, value)` pair on the value's TYPE and skips any other,
  so both fields were dropped, the catalog selector came up empty, and every SPEC this crate
  built was refused with `INVALID_PATH` — a vertex could not be created from Rust at all.
  Nothing here caught it: the drifted bytes decode and re-encode to themselves, so the
  conformance harness scored them `ok`, and this crate's own reader accepted whatever payload
  it found. Both halves are fixed, and both are now byte-pinned against shared vectors.

### Added

- **`spec/create-child` and `spec/conn-client-ws` conformance vectors**, shared with the C++
  and TypeScript cores and pinned in `tests/conformance_vectors.rs`
  (`spec_create_child`, `spec_conn_client_ws`, `spec_value_typed_fields_are_not_the_vector`).
  The `spec/` category did not exist before, which is why the drift above went unnoticed.

- **`structured::SettingValue` and `structured::settings_typed`** — build a SETTINGS record
  whose values are typed per key: `SettingValue::Value(&[u8])` for an opaque `VALUE`
  (integers little-endian, flags, blobs) and `SettingValue::Name(&str)` for a textual `NAME`.
  A reader looks a key up BY type, so a string written as a `VALUE` is invisible where a
  string is expected; before this, the crate could emit only the `VALUE` form and a
  string-valued setting such as a connection's `kind` / `addr` was inexpressible.
  `settings` is unchanged and is now the all-`Value` case of `settings_typed`.

- **`structured::settings_str`** — read a SETTINGS key back as a string, present only when
  the value child really is a `NAME`, mirroring the terminus's typed lookup.

- **`text_name`** (re-exported at the crate root) — a `NAME` node for a KEY or a string field
  VALUE that is not an address segment. `NAME` is the wire's only string node and spells both
  things; `name` enforces the addressing grammar because an address segment must satisfy it,
  while a string value routinely may not (an `addr` dotted quad contains `.`, which `name`
  rejects).

### Changed

- **`structured::spec_type_name` now reads the value TYPE, not just the payload.** A `type` /
  `name` field whose value child is not a `NAME` reads as `None`, matching the terminus's
  typed lookup. Previously any payload was accepted, which is what let this crate round-trip
  its own malformed SPEC green.

- **`structured::spec` validates its two fields the way the terminus does:** `name` must be a
  legal address segment (it becomes a path component), `type` need only be non-empty and
  within the 64-byte budget (a catalog selector is never addressed). A `child_name` that no
  terminus would accept is now refused at build time instead of on the wire.

  Two divergences from the C++ terminus remain, both pre-existing and both disclosed in the
  rustdoc rather than papered over — this release closes the **type** half of the parity, not
  all of it. (1) The readers' **walk** differs: the terminus consumes strict `(NAME key,
  value)` pairs, breaks at the first non-`NAME` key slot, and lets a later well-formed pair
  win, while `named_fields` resynchronises at every offset and `named_field` takes the first
  match — so a stray leading `VALUE` in a SPEC is fatal there and survivable here, and a
  re-stated SETTINGS key resolves to opposite values. (2) `name`'s segment predicate rejects
  `[` and `]`, which `valid_segment` deliberately admits as the address-index suffix form, so
  the builder refuses a `child_name` a terminus would accept.

## [0.8.0] — 2026-08-06

### Changed

- No Rust-binding-visible changes — version-lockstep release with `core` 0.8.0 (its new
  `for_each_vertex` / subscription-observer surface is C++ `core` API only).

## [0.7.1] — 2026-08-04

### Added

- **The RFC-0024 forwarding car's two vectors are pinned.** `fwd/fwd-bound-forward` and
  `fwd/fwd-bound-forwarded` — the same operation one bound hop apart — are round-tripped and
  byte-pinned by `fwd_bound_forward_is_one_hop_from_forwarded`, including the residual being
  the inbound element array minus its head and `src` growing canonically. No API change: this
  crate carries the shape and, having no router, never interprets an element.

- **`fwd::ParsedFwd` learns the RFC-0024 §5-§7 routing surface.** Two new fields —
  `mint_request` (the `op` byte's bit 7) and `dst_bound` (`dst` is a `PATH_REF`, not a `PATH`)
  — plus `fwd_op::OPCODE_MASK` (`0x3F`) and `fwd_op::FLAG_MINT_REQUEST` (`0x80`).
  **Behaviour change, and both halves matter for interop:** `parse_fwd_tlv` now masks the `op`
  byte before comparing it, so a mint-flagged operation parses as its plain opcode instead of
  falling through every arm; and it accepts a `PATH_REF` in the `dst` and `src` positions,
  which it previously rejected as `TypeMismatch`. An element is node-scoped, so this binding
  carries one and never interprets it.

- **`type_code::PATH_REF` (`0x14`) and the bound-path element codec
  ([RFC-0024](../../docs/spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §4).**
  New public items: `PathRefElement { index: u32, generation: u32 }`,
  `path_ref(&[PathRefElement]) -> Result<Tlv, BuildError>`,
  `path_ref_element(&[u8], usize) -> Option<PathRefElement>`, and the constants
  `PATH_REF_ELEMENT_BYTES` (8) and `MAX_PATH_REF_ELEMENTS` (255). `path_ref` answers
  `BuildError::TooManySegments` past the bound rather than truncating.
- **`decode` enforces the `PATH_REF` body shape.** For type `0x14`, `opt.pl` and `opt.ll` must
  be clear, the length must be a multiple of 8, and the element count must be ≤ 255; a
  violation is `Error::FrameInvalid`. **Behaviour change:** bytes that previously decoded as an
  opaque unknown-type TLV with type `0x14` now reject. `0x14` was unassigned, so no frame any
  libtracer version has emitted is affected.

## [0.7.0] — 2026-08-02

### Added

- **`path::admit_path_tlv(&Tlv) -> Result<(), BuildError>` (#773)** — the construction/admission
  gate for a *decoded* PATH: the encode-time MUST of `tlv_builders::path`
  (`children ≤ MAX_SEGMENTS` ∧ each child a valid NAME ∧ encoded body ≤ `MAX_PATH_BYTES`)
  evaluated over an already-parsed tree. Call it where a foreign PATH becomes an address a
  resolver will spell; do **not** call it on a `src` return route being forwarded on. The 255
  bound is unchanged and exact — the same inputs it accepted and rejected at the decode tier it
  accepts and rejects here.

- **`structured::DeliveryPolicy`, `structured::subscriber_policy`,
  `structured::subscriber_with_policy` and `structured::DELIVERY_POLICY_KEY`
  ([RFC-0022](../../docs/spec/rfcs/0022-qos-restructure.md) §3.A; #758)** — read and write one
  subscription's packed 16-bit delivery-policy word, the `SETTINGS{ NAME "delivery_policy"
  VALUE u16 }` child of a SUBSCRIBER. Delivery policy describes a **producer→subscriber
  relationship**, not the producer, so it travels on the subscribe-write rather than on the
  vertex. The bit layout mirrors the C++ core's `delivery_policy_t` byte for byte — bits 0–1
  reliability, 2–4 priority, bit 5 `durability_request` (`DeliveryPolicy::DURABILITY_REQUEST`),
  6–15 reserved. Reserved bits are round-tripped **verbatim and never interpreted** (§3.A says
  ignore, not reject), so a future sender's bits survive a hop through this binding.
  `subscriber_policy` reads the word **by NAME**, never by position — a `SETTINGS` child naming
  a different key (e.g. `delivery_compact`) is not the policy and yields the default.
  `subscriber_with_policy` emits **no** `SETTINGS` child for an all-zero policy, so a caller
  that does not ask for a policy produces bytes identical to `tlv_builders::subscriber`'s and
  to a pre-RFC-0022 sender's. Pinned to the `subscriber/policy-{absent,durability,reserved-bits}`
  conformance vectors, with the two ablations that bite recorded in the commit that added them.

### Changed

- **SEMVER-VISIBLE — `MAX_SEGMENTS` repriced 32 → 255 (RFC-0023, accepted; #767).** The exported
  constant (`libtracer::MAX_SEGMENTS`) changes value, so any consumer that mirrors it in an array
  size or a message must be recompiled. `split_path` / `tlv_builders::path` / `tlv_to_path` accept
  33–255 segments where they previously answered `BuildError::TooManySegments`; the error variant
  itself is unchanged and still maps to `tr::path::invalid`. Under the current NAME-TLV body
  encoding the `MAX_PATH_BYTES` (1024) check binds first at 204 segments, so `path_to_tlv` answers
  `BuildError::PathTooLong` — not `TooManySegments` — for a 205-segment path.
  ~~**Known misplacement, repriced not relocated:**~~ relocated below (#773).

- **SEMVER-VISIBLE — the accumulative PATH bounds left the decode tier (RFC-0023 §5.6; #773,
  landed in #778).**
  `path::tlv_to_path` no longer answers `BuildError::TooManySegments` or
  `BuildError::PathTooLong`: `docs/reference/05-protocol-tlvs.md` §"Enforcement of the PATH
  constraints" places the segment-count and total-length limits where an address is
  **constructed or admitted**, and states that "the codec does not enforce these constraints,
  and is not expected to". A caller that relied on `tlv_to_path` (or `fwd::fwd_dst_path` /
  `fwd::fwd_src_path` / `structured::subscriber_target_path`, which route through it) to reject
  an over-limit PATH must call the new `path::admit_path_tlv` instead. This closes RFC-0019
  §10(b): an accumulated `src` return route is legal at any byte-reachable depth, and decoding
  one no longer fails. The per-segment rules (1..64 bytes, no reserved character, NAME child
  type, UTF-8) stay in `tlv_to_path` — they are not accumulative, and they are what makes the
  rendered string re-splittable.

- **The `:field` selector examples no longer name a knob that was deleted
  ([RFC-0022](../../docs/spec/rfcs/0022-qos-restructure.md) §5 + amendment 1; #758).**
  `field::FieldLevel`'s module docs illustrated the two-scalar-level shape with
  `":settings.deadline_ns"`, then `":settings.history_keep_last"`; both names were removed from
  the protocol's `settings` container, and the example is now `":settings.app"` — the only
  two-level `settings` field that still resolves. **No API changed here**, and the FIELD grammar
  is untouched: `parse_field` still parses any syntactically valid selector. What changed is on
  the wire — a peer answers `tr::field::not_found` for the removed names, on read *and* on
  write, independently of the caller.

- **`path`'s module header names the byte bound's real string-side enforcer.** A doc-only
  correction following the accumulative-bounds relocation above; no behaviour change.

### Fixed

- **The three `subscriber/policy-*` conformance vectors were scored `ok` by a core that
  implemented none of them (#758).** The polyglot harness contract is
  `encode(decode(input.bin)) == input.bin`, which any well-formed TLV satisfies, so this
  binding — which contained no delivery-policy code at all — passed the policy vectors on
  structure alone. The behaviour is now claimed where it can be false: `DeliveryPolicy` above,
  plus three vector-bound tests and a bit-layout pin in `tests/conformance_vectors.rs`.

## [0.6.0] — 2026-07-23

## [0.5.0] — 2026-07-21

## [0.4.0] — 2026-07-09

### Removed

- **BREAKING — `MAX_DEPTH` is removed (RFC-0006).** Nesting depth is
  receiver-resource-bounded, never a constant: `decode`'s explicit work stack is
  a growable `Vec` (the host heap is this binding's decode resource), so the
  depth-cap reject is gone. `Error::TlvNestingTooDeep` remains registered
  ("exceeds the receiver's decode resources") for harness parity and peers'
  `ERROR` frames; `decode` itself no longer produces it.

## [0.3.0] — 2026-07-07

### Added

- **Typed protocol CODEC tier on top of the wire codec** — mirrors what the
  TypeScript client provides; every builder/parser reproduces and interprets the
  shared conformance vectors byte-for-byte with the C++ core.
  - `mod tlv_builders` — typed TLV builders (`value` / `value_opts` /
    `value_u8..u64`, `name`, `path`, `subscriber`, the `Opt::structured()` /
    `opt(..)` helpers), the `ValueOptions` selector, `BuildError`, and
    child-lookup accessors on `Tlv` (`first_child`, `children_of_type`,
    `child_at`, `payload_str`, `payload_uint`).
  - `mod path` — PATH string ⇄ TLV (`split_path`, `path_to_tlv`, `tlv_to_path`)
    with the addressing rules (1..64-byte segments, reserved chars `/ : . [ ] * ?`,
    ≤32 segments, ≤1024 total encoded bytes), cross-checked against
    `core/src/path.cpp`.
  - `mod error_registry` — the 15-entry RFC-0002 `tr::<concept>::<error>` registry
    (`ErrCode` with `code`/`path`/`from_code`/`from_path`/`severity`/`disposition`),
    ERROR-TLV parse/encode in both identity forms (`error_code`, `error_string`,
    `parse_error`), and STATUS helpers (`status_ok`, `status_with_errors`,
    `status_errors`). Kept separate from the codec-fault `Error` enum.
  - `mod field` — the `:field` selector: `parse_field` / `encode_field` /
    `field_tlv` / `parse_field_tlv`, `FieldLevel`, `FieldMode`.
  - `mod structured` — typed read/build for POINT, SETTINGS, ACL (NFSv4-style
    `Ace`), SUBSCRIBER, SPEC, and a generic NAME-tagged field reader
    (`named_fields`) covering the ROUTER envelope.
  - `mod fwd` — the FWD remote-operation envelope: `FwdRequest` / `encode_fwd` /
    `decode_fwd` / `parse_fwd_tlv`, `fwd_op` / `fwd_kind`, reply error
    code/path accessors, and the FWD error-path table (RFC-0004 §B/§D).
  - `tests/conformance_vectors.rs` — an in-crate structural conformance suite that
    loads each vector's `input.bin` **and** `expected.json` and asserts the decoded
    typed structure (closing the previous round-trip-only gap; 31 tests).

- **`type_code::FWD` (`0x0F`) and `type_code::FIELD` (`0x10`)** registered in the
  type-code registry (RFC-0004 / ADR-0035, slice 1). The two remote-operation
  frames are structured TLVs the generic codec already round-trips; no codec change.
  New cross-core conformance vectors under `tests/conformance/vectors/v1/{fwd,field}/`
  match the C++ and TypeScript cores byte-for-byte and are exercised by `diff_fuzz.py`.

- **Native, pure-Rust wire codec — a third independent core**
  ([#57](https://github.com/avatarsd-llc/libtracer/issues/57),
  [ADR-0028](../../docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)).
  A from-scratch `#![no_std]` (`alloc`-only, zero external dependencies) port of the
  protocol-v1 wire format — not an FFI shim over the C++ core. It cross-matches the
  C++ and TypeScript cores byte-for-byte on the shared conformance vectors
  (`tests/conformance/run-all.py`) and on random differential-fuzz frames
  (`tests/conformance/diff_fuzz.py`).
  - Public API: `decode(&[u8]) -> Result<Tlv, Error>`, `encode(&Tlv) -> Vec<u8>`,
    the CRC primitives `crc32c` / `crc16_ccitt`, and the owned model
    (`Tlv`, `Opt`, `Trailer`, `Timestamp`, `Crc`, `CrcWidth`, `Error`, `type_code::*`,
    `MAX_DEPTH`).
  - Decoder is bounds-/overflow-safe: an over-long length on a short buffer returns
    `Error::FrameTruncated` rather than reading out of bounds; nesting is parsed
    iteratively and capped at `MAX_DEPTH` (32).
  - `examples/conformance.rs` implements the shared harness contract (`--tap` /
    default TAP mode and `--roundtrip` differential-fuzz mode).
  - `examples/perf.rs` is the Rust core's codec perf bench — the **core-impl-lang**
    axis (Rust) of the cross-core perf matrix ([#96](https://github.com/avatarsd-llc/libtracer/issues/96),
    [ADR-0032](../../docs/adr/0032-continuous-cross-core-perf-conformance-matrix.md)).
    It times decode→encode roundtrips over the shared conformance vectors and emits
    the same 12-field `RESULT` line per vector as the C++ (`bench/bench_libtracer.cpp`)
    and TypeScript (`perf.mjs`) benches, with `system="rust-core"`, `mode="codec"`.
    Run with `cargo run --release --example perf -- tests/conformance/vectors/v1`.
