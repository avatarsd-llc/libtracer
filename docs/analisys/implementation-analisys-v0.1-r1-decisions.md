# libtracer v0.1 — Decisions Required (companion to r1 analysis)

> **Purpose**: one question per resolvable inconsistency or under-specified rule from [implementation-analisys-v0.1-r1.md](implementation-analisys-v0.1-r1.md). My recommendation is pre-selected with `[x]`; alternatives carry `[ ]`. To accept the recommendation, leave it as-is. To pick an alternative, move the `[x]` to your choice. Use the **Notes** line to refine wording or add constraints.
>
> When all decisions are filled in, hand this file back and I will apply the consolidated edit pass across the spec, reference docs, and code stubs.
>
> **Conventions**:
> - Questions are grouped by analysis cycle and numbered to match the analysis sections (`Q1.1` → r1 §1.1).
> - "Touches" lines name the files the decision will edit. Use them to spot-check blast radius.
> - Multi-part questions ask one thing per sub-bullet (`Q5.7a`, `Q5.7b`, …) so each has its own `[x]`.
> - Where a question is a **yes/no**, the recommended option is "yes" unless flagged otherwise.

---

## Cycle 1 — Static-analysis decisions

### Q1.1 — PATH TLV `opt = 0x50` literal mismatch

The bit table says `0x50 = PL + CR`. Three docs label `0x50` as "PL only" in PATH literals. Which is the source of truth?

- [x] (A) **Recommended.** Change every PATH literal that claims "PL only" to `0x40`. Bit table is correct. Affects: 03-addressing.md §static path handles, 05-protocol-tlvs.md §PATH §static byte literal (sensor/temp + camera/frame), 06-user-data-packing.md MCU recipe. 01-data-format.md §worked-frame-examples PATH already uses `0x50` *with* an outer CRC trailer — leave that one alone.
- [ ] (B) The bit table is wrong; bit positions for PL/CR are different. (Implausible — every other example in the suite uses the documented bit positions.)
- [ ] (C) Keep `0x50` everywhere; PATH always carries an outer CRC. Promote `opt.CR=1` to mandatory for PATH. (Adds 4 bytes to every static handle; defeats the 22-byte minimum.)

**Touches**: 03-addressing.md, 05-protocol-tlvs.md, 06-user-data-packing.md, conformance vectors.

**Your decision**: leave `[x]` on the row you accept.
**Notes**:

---

### Q1.4 — Reserved-bit policy for `opt`

Reserved bits 0 and 7 are MUST-be-zero, MUST-reject. Should this hold forever, or are they reserved for v0.1-minor evolution?

- [x] (A) **Recommended.** Forever-frozen for v0.1. Forward extension goes through type codes `0x0E–0x7F` only. Add an explicit sentence in 01-data-format.md §options bitfield.
- [ ] (B) Reserved-but-thawable: a v0.1 minor revision MAY allocate them, with new MUST-rules for receivers. (Forces all receivers to upgrade; breaks the "wire format is one-shot" claim.)

**Touches**: 01-data-format.md §options bitfield, 04-overview's "load-bearing claim 3."
**Your decision**:
**Notes**:

---

### Q1.5 — Wildcards in path EBNF

EBNF excludes `*` from `name`; wildcard examples in §wildcards aren't grammatical under that EBNF.

- [x] (A) **Recommended.** Add a `subscriber-path` non-terminal in 03-addressing.md §path syntax that admits `*`, `**`, and `[*]` only at whole-segment positions. Concrete `path` (read/write/await arg) keeps the strict grammar.
- [ ] (B) Loosen `name` to admit `*`/`**` as full-segment alternatives directly. (Conflates segment grammar with wildcard semantics; harder for tooling.)

**Touches**: 03-addressing.md §path syntax.
**Your decision**:
**Notes**:

---

### Q1.6 — ROUTER child-parity rule

ROUTER's children are alternating NAME/value pairs ending with `NAME "data" + wrapped TLV`. Behaviour on malformed ROUTER (odd parity, missing `data` tag, two consecutive NAMEs) is unspecified.

- [x] (A) **Recommended.** Normative MUST: receivers reject malformed ROUTER with `STATUS=ERROR(INVALID)` (new code from Q1.10). Drop, do not forward.
- [ ] (B) Best-effort: skip the malformed pair, continue parsing remaining metadata, dispatch the wrapped data anyway. (Forwards potentially attacker-controlled TLVs; no.)
- [ ] (C) Receiver-defined. (Current state; cross-impl divergence guaranteed.)

**Touches**: 05-protocol-tlvs.md §ROUTER, 07-host-embedding.md §cycle handling.
**Your decision**:
**Notes**:

---

### Q1.7 — Nested-trailer behaviour

A structured TLV (PL=1) contains children that have their own `opt.CR/opt.TS` trailers. When a bridge re-emits the outer, what happens to the inner trailers?

- [x] (A) **Recommended.** Inner trailers are part of the outer's payload bytes; outer's strip-on-ingress / append-on-egress NEVER touches children. Children's bytes are invariant from publication to all subscribers (including bridge re-emit).
- [ ] (B) Bridges recompute every nested trailer (CRC, TS) at re-emit. (Defeats the "bytes invariant" property; subscribers can't compute stable hashes.)

**Touches**: 01-data-format.md §append-only-at-egress, 02-graph-model.md §the trailer enables payload-bytes invariance.
**Your decision**:
**Notes**:

---

### Q1.8 — Empty structured TLV (`length=0, opt.PL=1`)

Is a structured TLV with no children valid?

- [x] (A) **Recommended.** Valid. Useful for "an empty SETTINGS means no overrides," "an empty POINT means leaf with no metadata." Receivers iterate zero children harmlessly.
- [ ] (B) Invalid. `length=0` always means `opt.PL=0`. Receivers reject with `INVALID`.

**Touches**: 01-data-format.md §minimum frame, 05-protocol-tlvs.md (per-type rules).
**Your decision**:
**Notes**:

---

### Q1.9 — Address-shift implicit `expected_count`

Without `:settings.address_shift.expected_count`, the assembler treats `largest-observed-index + 1` as N. A corrupted/malicious index of `0xFFFE` causes a memory-amplification stall.

- [x] (A) **Recommended.** Bound implicit N at 256 by default; require an explicit manifest (or `:settings.address_shift.expected_count`) for groups larger. Receiver of an out-of-bound index without explicit declaration MUST emit `STATUS=ERROR(ADDRESS_SHIFT_GAP)` and discard the group.
- [ ] (B) Always require an explicit MANIFEST type code (Q4.3) for any group N>1. No implicit N. (Cleanest, but requires every publisher to learn the manifest pattern.)
- [ ] (C) Cap at a different value (state in Notes).

**Touches**: 03-addressing.md §loss detection, 05-protocol-tlvs.md §MANIFEST (if added).
**Your decision**:
**Notes**:

---

### Q1.10 — Error code registry assignments

Several error names appear in normative MUSTs without registry entries. Assign them now:

- [x] **Q1.10a** — `0x0F INVALID` (general structural invalidity, distinct from `INVALID_PATH`).
- [ ] Alternative name (state in Notes).

- [x] **Q1.10b** — `0x10 PEER_ID_COLLISION` (two distinct peers presenting the same peer-id).
- [ ] Alternative name (state in Notes).

- [x] **Q1.10c** — Reserve `0x11–0x7F` for future core; document `0x80–0xFF` as user-definable (already implied; make explicit).
- [ ] Alternative split.

**Touches**: 05-protocol-tlvs.md §error code registry, every doc that names these errors.
**Your decision**:
**Notes**:

---

### Q1.11 — Path canonicalization (NFC)

The current text says implementations MAY normalize to NFC. This is a cross-impl interop bomb.

- [x] (A) **Recommended.** Normative **MUST NOT normalize**. Paths are byte-equal sequences. Senders that want NFC do it once at registration. Conformance vectors include a non-NFC path that dispatches identically to its bytes.
- [ ] (B) Normative **MUST normalize to NFC** at parse boundary. (Costs ICU on MCU; probably wrong call.)
- [ ] (C) Status quo: MAY. (Guarantees implementations diverge.)

**Touches**: 03-addressing.md §path canonicalization, conformance vectors.
**Your decision**:
**Notes**:

---

### Q1.12 — Field-chain edge cases

Enumerate the valid/invalid forms for `:` field paths. My proposed list — confirm or amend:

- [x] **Q1.12a** — `:subscribers[3]` reads/writes the SUBSCRIBER at slot 3. **Valid.**
- [x] **Q1.12b** — `:subscribers[3].liveness.last_seen_ns` reads/writes the nested field. **Valid.**
- [x] **Q1.12c** — `:subscribers[]` (read = list, write = append). **Valid.**
- [x] **Q1.12d** — `:subscribers[].liveness` is **invalid** (no slot to read liveness of). Reject with `INVALID_PATH`.
- [x] **Q1.12e** — `:settings.` (trailing dot) is **invalid**. Reject with `INVALID_PATH`.
- [x] **Q1.12f** — `:` alone (empty field-chain after separator) is **invalid**. Reject with `INVALID_PATH`.

To override any line, change `[x]` to `[ ]` and add Notes.
**Touches**: 03-addressing.md §field-path resolution.
**Your decision** (per sub-item):
**Notes**:

---

### Q1.13 — Discoverable vertex role

Should a peer be able to discover whether `/canvas` is Mode-A (stored) or Mode-B (sink-with-model) over the wire?

- [x] (A) **Recommended.** Optional `:role` field (enum: stored / stream / sink-with-model / computed / proxy / aggregate / live-mmio). Absent ⇒ peers assume `stored`. Documented in 02-graph-model.md as a peer-discoverable hint, NOT a contract.
- [ ] (B) Status quo: roles are deliberately invisible; the schema's writable fields imply the role. (Doc 11's current stance; fine if you want consumers to never depend on role.)
- [ ] (C) Mandatory `:role`. (Forces every vertex to declare; over-engineered for many cases.)

**Touches**: 02-graph-model.md §schema and field discipline, 11-vertex-roles-and-aggregation.md.
**Your decision**:
**Notes**:

---

## Cycle 2 — Cross-language portability decisions

### Q2.1 — C23 minimum compiler matrix

Pin a minimum compiler version per target tier.

- [x] (A) **Recommended.**
  - Hosted Linux/macOS: GCC 13+, Clang 18+.
  - ESP-IDF: 5.3+ (which bundles GCC 13).
  - arm-none-eabi: 14.x+.
  - MSVC: not supported until C23 lands; document as a known gap.
- [ ] (B) Different cutoffs (state in Notes).

**Touches**: README.md, plans/02-roadmap-weeks-1-to-8.md, build docs.
**Your decision**:
**Notes**:

---

### Q2.2 — `LIBTRACER_THREAD_MODE` macro

Introduce a build flag that gates atomic vs single-threaded codepaths.

- [x] (A) **Recommended.** Yes. `LIBTRACER_THREAD_MODE = single | multi` (default `multi`). `single` mode replaces atomics with plain loads/stores; subsumes the old `LIBTRACER_NO_ATOMIC=ON`.
- [ ] (B) Keep `LIBTRACER_NO_ATOMIC` as-is (boolean toggle).
- [ ] (C) Always atomic; don't allow opt-out. (Fails Cortex-M0 without LDREX/STREX.)

**Touches**: core/include/libtracer/* (rewrite), build docs.
**Your decision**:
**Notes**:

---

### Q2.3 — Rust binding strategy

- [x] (A) **Recommended.** Two crates: `libtracer-sys` (raw FFI bindgen against the C ABI) and `libtracer` (idiomatic safe wrapper). Plan a pure-Rust `libtracer-core` impl for v0.2 as the second-implementer gate.
- [ ] (B) Pure-Rust from day 1; FFI is the second-implementer gate. (Slower to first ship; doubles the impl surface during v0.1.)
- [ ] (C) FFI only; no pure-Rust. (Forfeits the second-implementer milestone.)

**Touches**: bindings/rust/, plans/02-roadmap.
**Your decision**:
**Notes**:

---

### Q2.4 — TS BigInt requirement for u64 timestamps

- [x] (A) **Recommended.** TS impl MUST use BigInt for u64 absolute trailer TS. Document in conformance vectors. No `Number`-truncation.
- [ ] (B) Use `Number` and accept ms-precision (lose ns-resolution). (Wrong shape for ns-based timestamps.)

**Touches**: bindings/typescript/, conformance vector docs.
**Your decision**:
**Notes**:

---

### Q2.5 — Big-endian target support

- [x] (A) **Recommended.** Explicitly supported but not v0.1-CI-tested. Add a sentence in 01-data-format.md §endianness: "Implementations on big-endian hosts MUST byte-swap all multi-byte fields on parse and emit."
- [ ] (B) Explicitly **un**supported. v0.1 is little-endian targets only. Big-endian is a v1.x scope question.
- [ ] (C) Add to v0.1 CI matrix. (Costs CI time; no current users requesting it.)

**Touches**: 01-data-format.md §endianness.
**Your decision**:
**Notes**:

---

## Cycle 3 — Adversarial / corner-case decisions

### Q3.1 — `boot_id` in ROUTER metadata + dedup key

Add `boot_id` (per-boot UUID) to ROUTER and to bridge dedup key. Kills reboot-clock-zero and post-NTP-jump dedup poisoning in one stroke.

- [x] (A) **Recommended.** Yes. Add `NAME "origin_boot_id" VALUE <16 bytes>` to ROUTER. Recent-set key becomes `(origin_peer_id, origin_boot_id, origin_timestamp)`. Bumps ROUTER metadata by 28 bytes per hop (16-byte UUID + NAME framing).
- [ ] (B) Use a 4-byte boot counter instead of UUID. Smaller, but collisions are possible across devices. (Per-device counters reset, so collisions remain.)
- [ ] (C) No boot_id; document the reboot-clock-zero failure mode as a known limitation requiring an RTC.

**Touches**: 05-protocol-tlvs.md §ROUTER, 07-host-embedding.md §cycle handling, conformance vectors.
**Your decision**:
**Notes**:

---

### Q3.2 — Re-entrant write inside callbacks

A subscriber's callback writes back into the graph; potential infinite recursion or stack overflow.

- [x] (A) **Recommended.** Work-queue dispatch: `tracer_write` from inside a callback enqueues the secondary write; the dispatcher processes it after current fan-out completes. Callbacks for the same subscriber serialized (see Q5.4). Document in 04-communication-flows.md §re-entrant write.
- [ ] (B) Depth-cap: per-thread re-entry counter; depth > 16 returns `STATUS=ERROR(NESTING_TOO_DEEP)`. Synchronous semantics preserved; risk of unbounded stack pinned to 16 frames.
- [ ] (C) Forbid re-entrant writes; callbacks return error if they call `tracer_write`. (Too restrictive.)

**Touches**: 04-communication-flows.md, dispatcher implementation.
**Your decision**:
**Notes**:

---

### Q3.3 — Security clarification language

CRC is not adversarial integrity. Spec uses "validate" / "verify" / "integrity" loosely.

- [x] (A) **Recommended.** Add a §security-clarifications block to 01-data-format.md stating CRC is for bit-flip detection, not adversarial integrity. Audit docs for "verify" / "integrity" in CRC context; replace with "frame check."
- [ ] (B) Keep the language; assume readers know CRC ≠ MAC. (Some won't.)

**Touches**: 01-data-format.md, audit pass across all reference docs.
**Your decision**:
**Notes**:

---

### Q3.5 — Address-shift group key

Currently keyed on `ts` only. Two publishers with the same ts merge into a Frankenstein.

- [x] (A) **Recommended.** Group key is `(origin_peer_id, ts)`. Subscriber wildcard match metadata exposes `origin_peer_id` so concurrent groups stay distinct.
- [ ] (B) Document as single-publisher invariant; assemblers MAY trust ts alone. (Silent corruption in clock-aligned multi-publisher cases.)

**Touches**: 03-addressing.md §address-shift slicing.
**Your decision**:
**Notes**:

---

### Q3.6 — Wildcard subscriber cap

- [x] (A) **Recommended.** Add `:settings.max_wildcard_subscribers` (default 16, per-vertex). Wildcards rooted at `/` (catch-all) are gated by the `:acl` field — only subscribers holding the appropriate capability may register `/**`.
- [ ] (B) Cap-only, no ACL gate. (Lets one privileged subscriber lock the slot.)
- [ ] (C) ACL-only, no cap. (Cap is still useful as a runtime backstop.)
- [ ] (D) Neither; status quo "guidance" only.

**Touches**: 02-graph-model.md §schema discipline, 03-addressing.md §match cost.
**Your decision**:
**Notes**:

---

### Q3.7 — Segment pointer discipline (refcount ABA)

- [x] (A) **Recommended.** Add normative §segment pointer discipline to 02-graph-model.md or 08-views-and-ownership.md: any thread holding a segment pointer MUST hold a refcount; acquisitions from shared structures atomically increment.
- [ ] (B) Status quo: implicit Boost-intrusive-ptr discipline.

**Touches**: 02-graph-model.md or 08-views-and-ownership.md.
**Your decision**:
**Notes**:

---

### Q3.9 — Peer-ID collision response

- [x] (A) **Recommended.** Bridge that observes two distinct identity bindings for the same peer-id MUST refuse to forward and emit `STATUS=ERROR(PEER_ID_COLLISION)` (Q1.10b code). Combined with Q3.1's boot_id, even genuine peer-id reuse is distinguishable.
- [ ] (B) SHOULD emit; continue forwarding. (Current language; soft.)
- [ ] (C) Bridge picks one and ignores the other. (Implementation-defined; cross-impl divergence.)

**Touches**: 07-host-embedding.md §bridge identity.
**Your decision**:
**Notes**:

---

### Q3.10 — Append race on `:subscribers[]`

- [x] (A) **Recommended.** Normative MUST: append-form writes (`[]` index) are serialized at the vertex's atomic-update boundary; concurrent appends MUST receive distinct slot indices. Implementations report the assigned slot via the SUBSCRIBER's `subscriber_id` field.
- [ ] (B) Last-write-wins on slot N; documented as expected behaviour. (Loses appends silently.)

**Touches**: 03-addressing.md §reading vs writing array slots.
**Your decision**:
**Notes**:

---

### Q3.11 — Schema mutability

- [x] (A) **Recommended.** A vertex's schema is immutable for its lifetime. To change schema, application MUST unregister (subscribers see `STATUS=ERROR(NOT_FOUND)`) and re-register; subscribers re-subscribe.
- [ ] (B) Allow mutation in place; emit `STATUS=ERROR(SCHEMA_CHANGED)` (new error code) to subscribers; they may resubscribe lazily. (Cleaner UX but more complex.)
- [ ] (C) Status quo: not specified.

**Touches**: 02-graph-model.md §schema discipline.
**Your decision**:
**Notes**:

---

### Q3.13 — Unsubscribe grace window

- [x] (A) **Recommended.** Add `:settings.unsubscribe_grace_window_ms` (default 100). TLVs queued more than this many ms after unsubscribe MUST be dropped, regardless of queue space.
- [ ] (B) Different default (state in Notes).
- [ ] (C) Status quo: unbounded leak permitted.

**Touches**: 04-communication-flows.md §unsubscribe.
**Your decision**:
**Notes**:

---

## Cycle 4 — Architecture-rationale tightenings

### Q4.1 / Q4.11 — Central type-code registry for `0x80–0xFF`

- [x] (A) **Recommended.** Add `docs/spec/type-code-registry.md` as a markdown table; projects PR their use. Recommend a 4-byte project-magic prefix at the start of user-range payloads as a backstop.
- [ ] (B) No registry; magic-prefix convention only. (Status quo; collisions go undetected.)
- [ ] (C) IANA-style external registry. (Overkill for v0.1.)

**Touches**: docs/spec/type-code-registry.md (new), 05-protocol-tlvs.md §user range.
**Your decision**:
**Notes**:

---

### Q4.3 — Reserve `0x0E MANIFEST` in v0.1

- [x] (A) **Recommended.** Reserve `0x0E MANIFEST` now in 05-protocol-tlvs.md, even if implementation lands in v1.x. Define the structure: `MANIFEST { NAME "expected_count" VALUE u32, NAME "publisher" PATH, NAME "ts" TIME }`. Avoids registry churn later.
- [ ] (B) Defer. Allocate when first implemented. (Risk of someone else taking `0x0E` in `0x0E–0x7F`.)

**Touches**: 05-protocol-tlvs.md §reserved range, 03-addressing.md §address-shift.
**Your decision**:
**Notes**:

---

### Q4.4 / Q4.9 — Move `bridge` to "required-when-≥2-transports"

- [x] (A) **Recommended.** Bridge module is required only when ≥2 transports loaded. P0 (in-process) and P1 (single-transport leaf) builds don't link it. Updates 10-module-catalog.md profile table accordingly.
- [ ] (B) Status quo: bridge always required. (Wastes flash on RC-car-style P1 builds.)

**Touches**: 10-module-catalog.md §required modules per conformance profile.
**Your decision**:
**Notes**:

---

### Q4.5 — `:role` discovery field

(Same question as Q1.13; combined here for symmetry.)

- [x] Same answer as Q1.13.
- [ ] Different (state in Notes).

**Your decision**:
**Notes**:

---

### Q4.10 — L0..L5 cheat sheet

- [x] (A) **Recommended.** Add a one-page cheat sheet at the top of 00-overview.md showing one TLV's byte path through L0..L5 with each layer's contribution. Sourced from the DMA→ADC trace already in 08-views-and-ownership.md.
- [ ] (B) Status quo: leave the trace buried in doc 08.

**Touches**: 00-overview.md.
**Your decision**:
**Notes**:

---

### Q4.12 — Smallest-encoding rule

- [x] (A) **Recommended.** Promote SHOULD to MUST: senders MUST use the smallest `LL/CW/TF` that fits the value. Conformance vectors test rejecting unnecessarily-large encodings.
- [ ] (B) Keep as SHOULD; document the bandwidth waste. (Cross-impl: some senders waste 2–8 bytes/frame.)

**Touches**: 01-data-format.md §interop minimal vs feature-rich.
**Your decision**:
**Notes**:

---

## Cycle 5 — Unasked questions

### Q5.1 — Conformance vector format

- [x] (A) **Recommended.** Per-test directory under `tests/conformance/vectors/v1/<category>/<name>/` with three files:
  - `input.bin` — raw bytes the parser receives.
  - `expected.json` — parsed structure (machine-readable).
  - `description.md` — human description and rationale.
  Categories: `framing`, `path`, `tlv-types`, `errors`, `crc`, `address-shift`, `router-dedup`.
- [ ] (B) Different format (state in Notes).

**Touches**: tests/conformance/, docs/spec/v1.md §4.
**Your decision**:
**Notes**:

---

### Q5.2 — v0.0 → v0.1 migration note

- [x] (A) **Recommended.** Add a §migration block in `docs/spec/v1.md` listing the wire breaks: header size 8→4/6, CRC trailer-positioned + CRC-32C-default, length u32→u16/u32, opt redefined, `LIST=0x05` retired, NAME drops NUL, paths via handle. State explicitly: no v0.0 peer interoperates with v0.1.
- [ ] (B) Skip; v0.0 has no users per R5. (Existing pre-release builds in PlatformIO/Arduino/ESPHome may have leaked.)

**Touches**: docs/spec/v1.md, README.md changelog.
**Your decision**:
**Notes**:

---

### Q5.3 — `/_diag` namespace

- [x] (A) **Recommended.** Normative `/_diag/...` namespace per node, with these mandatory paths:
  - `/_diag/dispatcher:writes_total`
  - `/_diag/dispatcher:writes_dropped[{drop_reason}]`
  - `/_diag/bridge[N]:dedup_hits_total`
  - `/_diag/bridge[N]:dedup_evictions_total`
  - `/_diag/transport[N]:bytes_in_total`, `:bytes_out_total`
- [ ] (B) Different namespace (e.g., `/sys/...`, `/_libtracer/...`). State in Notes.
- [ ] (C) Implementation-defined; not normative. (Cross-impl divergence; tools break.)

**Touches**: 02-graph-model.md or 04-communication-flows.md (new subsection), 10-module-catalog.md.
**Your decision**:
**Notes**:

---

### Q5.4 — Threading model contract

- [x] (A) **Recommended.** Normative §threading model in 04-communication-flows.md:
  1. Subscriber callbacks invoked on implementation-defined thread.
  2. Callbacks for the same subscriber serialized (no concurrent invocations).
  3. Callbacks MAY call read/write/await; re-entrant writes follow Q3.2 (work-queue).
  4. View bytes valid for callback lifetime; implementation may release after return.
- [ ] (B) Different rules (state in Notes).
- [ ] (C) Implementation-defined. (Cross-impl divergence guaranteed.)

**Touches**: 04-communication-flows.md.
**Your decision**:
**Notes**:

---

### Q5.5 — Persistence / replay contract (v0.1 baseline)

- [x] (A) **Recommended.** Document in 04-communication-flows.md §recording-and-replay:
  - Recorder preserves per-source FIFO order.
  - Replay is a fresh write stream from the recorder's identity (NOT the original publisher's — preserves dedup safety).
  - Bridged TLVs MAY be recorded twice across separate recorders; dedup `(origin_peer_id, boot_id, ts)` on replay.
  - Sink-with-model vertex replay reconstructs state by replaying writes; missing writes ⇒ divergent model (known limitation).
- [ ] (B) Defer entirely to v1.x. (Wire format may need to change to support a stronger contract.)

**Touches**: 04-communication-flows.md.
**Your decision**:
**Notes**:

---

### Q5.7 — Resource-limits defaults

Confirm or adjust each default (per-implementation-configurable):

- [x] **Q5.7a** — Vertices per node default cap: **1024**.
- [x] **Q5.7b** — Subscribers per vertex default cap: **64**.
- [x] **Q5.7c** — Wildcard subscribers per topic default cap: **16** (matches Q3.6).
- [x] **Q5.7d** — Recent-set entries per bridge default cap: **8192**.
- [x] **Q5.7e** — Pending await waiters per vertex default cap: **8**.
- [x] **Q5.7f** — Subscriber queue depth default cap: **32**.

To override any default, change `[x]` to `[ ]` and put your number in Notes.
**Touches**: 02-graph-model.md or 04-communication-flows.md (new subsection).
**Your decision** (per sub-item):
**Notes**:

---

### Q5.8 — Dynamic handle lifecycle for peer-mounted paths

- [x] (A) **Recommended.** Handles for interpolated peer-mounted paths remain valid for the lifetime of registration, regardless of peer reachability. Writes through unreachable handles return `STATUS=ERROR(TRANSPORT_DOWN)`. Bytes never change; only resolution outcome does.
- [ ] (B) Handle invalidates when peer disappears; resubscribe required on reconnect. (More state to manage.)

**Touches**: 03-addressing.md §init-time registration.
**Your decision**:
**Notes**:

---

### Q5.10 — Scope-clarity sentence in 00-overview

- [x] (A) **Recommended.** Add at top of 00-overview.md: "**v0.1 scope.** libtracer v0.1 is the embedded-and-LAN data plane and the HPC control plane. It is not the HPC data plane; that role belongs to RDMA / NCCL / GPUDirect, which libtracer negotiates as a control plane."
- [ ] (B) Different wording (state in Notes).
- [ ] (C) Skip; doc 00-vision-and-reality-check.md already covers it. (Most readers don't reach the plans/.)

**Touches**: 00-overview.md.
**Your decision**:
**Notes**:

---

### Q5.11 — v0.1 minimum security posture

- [x] (A) **Recommended.** Add `security_required` flag at node init (default `false` for v0.1). When `false`, node logs a startup warning. ESPHome/PlatformIO/Arduino integration packages override to `true` once `security_*` modules ship; for v0.1 they ship with a documented "unsafe by default" warning.
- [ ] (B) `security_required = true` mandatory for any node with a non-loopback transport in v0.1; refuses to start. (Blocks v0.1 ship until `security_*` exists.)
- [ ] (C) No flag; security is silently optional. (Status quo; production deployments accidentally shipped insecure.)

**Touches**: 04-communication-flows.md, integrations/*/README.md.
**Your decision**:
**Notes**:

---

### Q5.12 — Module ABI semver

- [x] (A) **Recommended.** Declare the C reference ABI semver-stable from week 4. Export `tracer_abi_version` symbol; bump on every break. Document policy in plans/05-modules-transport-and-discovery.md §module ABI.
- [ ] (B) ABI is implementation-defined per [00-overview.md](../reference/00-overview.md#L172); no versioning. (Third-party transports break unpredictably.)

**Touches**: plans/05-modules-transport-and-discovery.md, core/include/libtracer/*.
**Your decision**:
**Notes**:

---

### Q5.13 — Integration package profile defaults

- [x] (A) **Recommended.** Each integration package documents its loaded profile + modules + security posture:
  - PlatformIO: P1 default; user picks transports via lib-deps.
  - ESPHome: P1 default with `transport_wifi` + `discovery_mdns`; warns if no `security_tls`.
  - Arduino: P0 default with `transport_serial`.
- [ ] (B) Different defaults (state in Notes).
- [ ] (C) Defer; integration packages document themselves.

**Touches**: integrations/*/README.md.
**Your decision**:
**Notes**:

---

### Q5.14 — Freeze-gate criteria

- [x] (A) **Recommended.** A reference doc section moves draft→frozen when:
  1. Its corresponding architecture-review P0 items are resolved.
  2. Conformance vectors exist for it.
  3. The reference impl passes those vectors.
  4. A second-implementer review passes (per current README §promotion rule).
- [ ] (B) Different gate criteria (state in Notes).

**Touches**: docs/reference/README.md §promotion rule.
**Your decision**:
**Notes**:

---

### Q5.15 — Cross-impl interop CI

- [x] (A) **Recommended.** Add `tests/interop/` with a docker-compose harness running a small fleet (1 ESP32 simulator, 1 Linux router, 1 browser-WS-client) sending a defined TLV set to each other on every PR.
- [ ] (B) Defer to v0.2; v0.1 ships with offline conformance vectors only.

**Touches**: tests/interop/ (new), CI config.
**Your decision**:
**Notes**:

---

### Q5.16 — Graceful shutdown sequence

- [x] (A) **Recommended.** Normative §shutdown sequence in 04-communication-flows.md:
  1. App calls `tracer_shutdown()`.
  2. Bridges emit `STATUS=ERROR(TRANSPORT_DOWN)` to subscribers.
  3. New writes return `ERROR=TRANSPORT_DOWN`.
  4. Pending writes drain (bounded by `:settings.shutdown_grace_ms`, default 1000).
  5. Subscriber callbacks complete; segments release.
  6. Module unload in reverse-init order.
- [ ] (B) Different sequence (state in Notes).
- [ ] (C) Implementation-defined.

**Touches**: 04-communication-flows.md.
**Your decision**:
**Notes**:

---

### Q5.17 — "When NOT to use libtracer" subsection

- [x] (A) **Recommended.** Add to 00-overview.md:
  > Don't use libtracer if you need:
  > - Cluster consensus or distributed transactions.
  > - Sub-µs guaranteed latency on multi-core Linux.
  > - Production-grade security in v0.1.
  > - 22-policy DDS QoS.
  > - HPC data plane >10 GB/s.
  > - ROS / DDS bridge.
  > - Schema evolution beyond re-registration.
- [ ] (B) Different list (state in Notes).
- [ ] (C) Skip; non-goals are scattered across docs already.

**Touches**: 00-overview.md.
**Your decision**:
**Notes**:

---

## Cycle 1 (continued) — the big code question

### Q1.2 — Code rewrite scope and timing

The existing `core/include/libtracer/*` is v0.0-shaped. The decision below sets the scope and timing of the rewrite.

- [x] (A) **Recommended.** Full rewrite in steps:
  1. Delete `tlv_vector.hpp`, `tlv_string.hpp`, `serdes.h`.
  2. Rewrite `tlv.h` with v0.1 wire format (4/6-byte header, trailer CRC, opt bits per spec, LIST gone).
  3. Add `view.h` (segment + view + rope), `tlv_cast.h` (in-place accessors), `path_handle.h` (.rodata literal macro).
  4. Replace `tracer.hpp` with `tracer.h` (C ABI: `tracer_read/write/await(handle, view)` only). C++ wrapper goes in a separate `tracer.hpp` with namespace.
  5. Delete `point_i::connect/disconnect`; rename duplicate `name_t` classes.
  6. Reconcile README "no-RTTI no-exceptions" with code: error-return APIs, `tl::expected<T,E>` in MCU builds, exceptions only in optional hosted wrappers.
- [ ] (B) Incremental: keep current files, add new ones alongside, deprecate old over several releases. (Forces ongoing v0.0/v0.1 ambiguity.)
- [ ] (C) Defer rewrite until after v0.1 spec freeze. (Spec freeze is gated on the rewrite passing conformance, so this is circular.)

**Touches**: all of `core/include/libtracer/`, CMakeLists, README.md.
**Your decision**:
**Notes**:

---

### Q1.3 — `tracer.hpp` duplicate `name_t` rename

If Q1.2 picks (A), the rewrite handles this. If Q1.2 picks (B) or (C), the rename still needs to happen now to make the file compile.

- [x] (A) **Recommended.** Rename in the rewrite (Q1.2-A) to: `value_t<T>`, `name_t`, `description_t`, `subscriber_t`, `path_t`, `point_t`, `error_t`, `status_t`, `acl_t`, `settings_t`, `time_t`, `router_t`. Add `cmake -B build` to CI as a compile sentinel.
- [ ] (B) Different naming convention (state in Notes).

**Touches**: core/include/libtracer/tracer.hpp.
**Your decision**:
**Notes**:

---

## Recommended fix sequence (after decisions)

Once you've filled in the table above, the application order I propose is:

1. **Spec micro-fixes** (~1 day): Q1.1, Q1.4, Q1.5, Q1.6, Q1.7, Q1.8, Q1.10, Q1.11, Q1.12.
2. **Spec normative additions** (~1 week): Q3.10, Q3.11, Q3.13, Q4.3, Q4.5/Q1.13, Q5.4, Q5.7, Q5.16, Q5.17.
3. **`boot_id` design** (2-3 days): Q3.1 + Q5.9 — touches ROUTER schema + recent-set + transport_label.
4. **Code rewrite scaffolding** (~1 week): Q1.2-A and Q1.3 — produce compiling but feature-light v0.1 skeleton.
5. **First conformance vectors** (1-2 weeks): Q5.1, Q5.2 — pinned to spec literally.
6. **Reference impl matches vectors**: existing 8-week roadmap, weeks 1-4.
7. **Second-implementer review**: per Q5.14.
8. **Promote to "frozen for v0.1"**: only when 1–7 done.

If you want to reorder or split, mark it in the **Sequence override** block below.

**Sequence override**:
**Notes**:

---

## How to hand this back

When done filling in:

1. Save in place (this file).
2. Tell me "decisions ready" — I'll diff `[x]` lines against the recommendations, group changes by file, and apply them in one consolidated edit pass.
3. If any decision needs more discussion before applying, leave both options unchecked and I'll ask follow-up questions.
