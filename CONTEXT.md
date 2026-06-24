# libtracer — Context Glossary

The canonical vocabulary for the libtracer protocol. It **supersedes** the former `docs/plans/99-glossary.md` (removed), which predated the [docs/reference/](docs/reference/) suite and carried pre-spec definitions. It tracks `docs/reference/` and the normative [docs/spec/v1.md](docs/spec/v1.md) — except where a maintainer decision recorded here resolves a conflict the reference had not (e.g. **Versioning**, **LIST**, **ERROR `0x06`/`0x08`**, **`io_dir_t`**), in which case the affected docs are brought into line with this file.

## Language

### Versioning

**Protocol version**:
The integer version of the libtracer wire format and the spec that defines it — currently **v1**. Immutable once frozen; a wire-incompatible change would be **protocol v2**. Not encoded per-frame — peers learn it at the **discovery layer** (mDNS `_libtracer._tcp` = v1, `_libtracer-v2._tcp` = v2).
_Avoid_: "wire format v0.1", "format version 0.1", per-frame version, `VR` / version bit.

**Release version**:
The implementation's own semantic version — a git tag / package version (`library.json`, `Cargo.toml`, `package.json`), currently **0.0.x**. Arbitrary and **decoupled** from the protocol version: a 0.0.x release is an early, partial implementation of **protocol v1**.
_Avoid_: calling this "the protocol version" or "the wire format version".

**Discovery-layer versioning**:
The mechanism that keeps incompatible protocol versions apart — a distinct service name / port / CAN-ID prefix per protocol version — used **instead of** a per-frame version field.

**Version bit (`VR`)**:
Does not exist. The wire format carries no per-frame version field; `opt` bit 7 is a forever-reserved MUST-be-zero bit (a `VR` version-bump bit was a rejected design, `01` §rejected designs).
_Avoid_: "`VR` bit", "version bit in `opt`", "`opt.VR`".

### Wire format (canonical per `docs/reference/01` + `05`)

**`opt` byte**:
The 1-byte options bitfield at offset 1 of every TLV; bits 7→0 are `R│PL│TS│CR│LL│CW│TF│R`. `PL`=payload-is-structured, `TS`=trailer-timestamp, `CR`=trailer-CRC, `LL`=length width, `CW`=CRC width, `TF`=timestamp form; bits 7 and 0 are reserved-must-be-zero (non-zero ⇒ reject as `INVALID`).
_Avoid_: the legacy `VR│PL│TS│FP│CR│reserved` layout; any `VR` (version) or `FP` (finite-pool) bit — neither exists.

**TLV header**:
A 4-byte header (`type` u8, `opt` u8, `length` u16 LE), or 6 bytes when `opt.LL=1` (`length` u32 LE). Integrity and wire-time live in the optional **trailer**, never the header.
_Avoid_: "8-byte header", "`crc` in the header", "`length: varint`".

**Length field**:
Fixed-width little-endian — u16 (default) or u32 (`opt.LL=1`). No u64; oversize payloads address-shift across `ep[0..N]`.
_Avoid_: "LEB128", "finite-pool length encoding" (both rejected, `01` §rejected designs).

**Trailer**:
Optional bytes appended at egress and stripped at ingress, leaving the payload byte-identical across hops. Carries an optional wire-time timestamp (`opt.TS`) and/or CRC (`opt.CR`).

**CRC**:
A trailer-resident frame check, gated by `opt.CR` — **CRC-32C** by default, CRC-16-CCITT when `opt.CW=1`, computed over payload + `trailer_ts` (header excluded). A bit-flip detector, not adversarial integrity.
_Avoid_: "XOR-16" (the stale code's checksum), "CRC in the header", "CRC always present".

**Structured TLV**:
Any TLV with `opt.PL=1`, whose payload is **purely** concatenated child TLVs; its **type code** declares what the children mean (PATH, SUBSCRIBER, POINT, ROUTER, …). There is no generic container type.
_Avoid_: "LIST", "type `0x05`", "graph-node-as-TLV LIST" — `0x05`/LIST is **retired**.

### Graph, addressing & API

**read / write / await**:
The entire data API — three calls, plus refcount management. There is **no** `connect` / `disconnect` / `subscribe` primitive.
_Avoid_: "connect", "disconnect", "subscribe()" as API verbs.

**Field-write**:
The control surface: subscriptions, QoS, ACLs, liveness are all writable fields addressed via the `:` separator on a vertex (e.g. `/sensor/temp:subscribers[3]`). Subscribing *is* writing a SUBSCRIBER into a `:subscribers[]` slot.

**Array-whole read / atomic multi-field write** (LIST replacement):
An array-whole read like `read('/x:subscribers[]')` returns a `PL=1` reply whose children are the element TLVs (SUBSCRIBER `0x04` for subscribers). An atomic multi-field write is a **SETTINGS (`0x0B`)** TLV. Neither uses a generic container.
_Avoid_: "returns a LIST", "write a single LIST TLV".

### Errors

**Error-code registry**:
The first **child TLV** of an ERROR (`0x08`) — a namespace separate from type codes. Canonical: `0x00` OK, `0x01` NOT_FOUND, `0x02` PERMISSION_DENIED, `0x03` INVALID_PATH, `0x04` TYPE_MISMATCH, `0x05` CRC_FAIL, `0x06` VERSION_MISMATCH, `0x07` BACKPRESSURE, `0x08` TIMEOUT, `0x09` TRANSPORT_DOWN, `0x0A` SCHEMA_NOT_FOUND, `0x0B` ADDRESS_SHIFT_GAP, `0x0C` TRUNCATED, `0x0D` NESTING_TOO_DEEP, `0x0E` PATH_IN_USE, **`0x0F` INVALID** (general structural invalidity: reserved-bit set, `type=0x00`, oversize length); `0x10–0x7F` reserved-core, `0x80–0xFF` user.

**`ERROR` (`0x08`)**:
A structured TLV (`opt.PL=1`); the error code is its **first child TLV** (a 1-byte VALUE), not a raw prefix byte — so the universal "`PL=1` ⇒ pure concatenated child TLVs" rule holds. Optional DESCRIPTION / VALUE children follow.

**`VERSION_MISMATCH` (`0x06`)**:
A discovery/bridge-level error — "peer advertised an incompatible protocol version." Not a frame-parse outcome (there is no per-frame version field to read).
_Avoid_: "`opt.VR` set higher than receiver supports".

### Modules & memory substrate

**Required modules**:
The modules every conforming node links (frame codec, path resolver, view/refcount machinery, router/dispatcher; bridge logic when ≥2 transports) — equivalently conformance profile **P0**. They are not architecturally privileged.
_Avoid_: "Core" as a noun for a fixed privileged build (the `core/` *directory* and "core type codes `0x01–0x1F`" are unaffected).

**`io_dir_t`**:
The L0 backend cache-coherency hook direction enum: `IO_DIR_DEVICE_TO_CPU` (DMA-in / RX ⇒ invalidate cache before the CPU reads HW-written bytes) and `IO_DIR_CPU_TO_DEVICE` (DMA-out / TX ⇒ clean cache so HW reads the CPU's last writes).
_Avoid_: the `IO_DIR_READ`/`IO_DIR_WRITE` spelling and any other integer-value set — one canonical spelling only.

**Segment / view**:
A **view** is a `{owner, offset, length}` window over a refcounted **segment** of backing memory (`segment_t` in the C ABI). Distinct from a **NAME segment** — a single `/`-separated path component, encoded as one NAME TLV (`0x02`).
_Avoid_: using bare "segment" for a path component; prefer "NAME segment" there and "view" for the L1 window.

## Flagged ambiguities (resolved)

- **"version"** — protocol = **v1** (integer, conformance-bearing, discovery-carried); release = **0.0.x** (arbitrary, git/package). "v0.1 is the wire format" is a category error → "protocol v1 is the wire format".
- **"LIST"** — retired (`0x05`); nesting is `opt.PL=1` + a purpose type byte; array reads concat element TLVs, multi-field writes use SETTINGS.
- **"Core"** — not a privileged unit; it means the **required modules** (profile P0).
- **"segment"** — overloaded; pin **view** (L1 window) vs **NAME segment** (path component).
- **`io_dir_t`** — `DEVICE_TO_CPU` / `CPU_TO_DEVICE` is canonical across reference 08/09/10.

## Example dialogue

> **Dev:** CI says we're on 0.0.1 but the spec says v1 — which is the breaking-change boundary?
> **Maintainer:** Different axes. **Protocol v1** is the boundary — it freezes and becomes immutable; a wire-incompatible change is **protocol v2** on a new discovery name. **0.0.1** is just this library's **release**; we'll ship 0.1, 1.0, 2.0 that all still speak **protocol v1**. No version bit on the wire — a peer learns we're v1 because we answer on `_libtracer._tcp`.
>
> **Dev:** I'm reading `:subscribers[]` — do I get a LIST back?
> **Maintainer:** No LIST anymore — `0x05` is retired. You get a structured (`PL=1`) reply whose children are SUBSCRIBER TLVs. To set several QoS fields at once you write a SETTINGS. The rule is always "`PL=1` plus a type byte that says what the children are."
