# Conformance harness contract

Every libtracer core (C++, TypeScript, Rust, …) is kept from drifting by running the
**same** vectors under `tests/conformance/vectors/v1/` and proving byte-identical
behavior. The mechanism and rationale are in
[ADR-0028](../../docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md).
This file is the contract a core implements to join that gate.

## The vectors

Each case is a directory under `vectors/v1/<category>/<case>/` containing:

| File | Meaning |
| --- | --- |
| `input.bin` | The canonical wire bytes for the case. |
| `expected.json` | Human-readable / cross-language decoded form (the spec of what `input.bin` *means*). |
| `description.md` | Prose describing the case. |

A **negative case** carries `reject.bin` **instead of** `input.bin` (exactly one of
the two, never both): bytes the codec MUST refuse to decode. Its `expected.json`
has a top-level `"reject": "<ERROR_NAME>"` naming the required decode error using
the same stable names as the `--roundtrip` `ERR:<name>` strings (e.g.
`FRAME_INVALID`). The distinct filename keeps every `input.bin` consumer (benches,
arena tests, the coverage audit) on valid frames only.

The **C++ reference is golden**: when the wire changes, it blesses new/updated
vectors and every other core must match them.

## What a vector gates — and what it does NOT

A vector gates the **codec, and only the codec**. The harness contract below is
`encode(decode(input.bin)) == input.bin`, which is satisfied by *any* well-formed TLV —
including one whose bytes no implementation ever produces or acts on. Passing a vector
therefore proves that a core can carry those bytes across its decoder and encoder without
losing a bit. It proves **nothing** about what the core does when it receives them.

This is not a gap to be closed by making the vectors cleverer; it is a boundary. Two
consequences follow, and both are load-bearing:

1. **A vector whose `description.md` states a behaviour is making a claim the harness
   cannot check.** Reverting the feature the behaviour names leaves every harness at a
   full score. So the claim must additionally be made where it *can* be false — a host
   test in the implementation that owns the behaviour, which fails when the behaviour is
   removed. Every behavioural vector below names that test.
2. **A core may score `ok` on a vector whose behaviour it does not implement at all.** A
   pure-codec binding round-trips a `SUBSCRIBER` carrying a `delivery_policy` word without
   ever interpreting a bit of it. That is a correct `ok` — the codec really is
   conformant — and it is why the score alone must never be read as "this core implements
   the feature". `run-all.py` reports codec agreement; the table below is where the
   behavioural gate lives.

Behavioural conformance is gated by **each implementation's own host suite**, not here.

### The execution model has ONE forwarder — a named limitation

The harness drives no topology at all: it decodes and re-encodes `input.bin`. There is therefore
no way to express a *routed* vector here, and the shortfall is not merely that a vector cannot
route — it is that until #419 **every FWD test in the C++ tree also had exactly one forwarder**.
That is how the multi-hop `dst` form could disagree three ways (reference docs, vectors,
implementation) for two weeks without a single red light: no artifact in the repo ever consumed
one address at two nodes.

`fwd/fwd-routed-two-mount` is delivered as a **codec-tier** vector for that reason — its bytes are
the origin frame of a real two-mount route, and the route itself is proven where it can fail, in
`core/tests/fwd_two_mount_test.cpp`. Read the vector's `ok` as "the codec carries these bytes",
never as "this core routes them".

### Behavioural vectors and their binding tests

| Vector | Behaviour claimed | Bound by |
| --- | --- | --- |
| `subscriber/policy-absent` | A `SUBSCRIBER` naming no `delivery_policy` behaves exactly as a pre-RFC-0022 one: no latch on join, later writes still delivered. | C++ `core/tests/qos_policy_test.cpp` — `test_policy_absent_is_todays_behaviour`, `test_conformance_vectors`, `test_replace_door_latches`. TS `bindings/typescript/packages/client/test/vectors.test.mjs`. |
| `subscriber/policy-durability` | Bit 5 set ⇒ the producer's latched value is delivered once on join, for **this** subscription and not its siblings — through the append door and the RFC-0009 §D.1 replace door alike. | C++ `qos_policy_test.cpp` — `test_policy_durability_is_per_subscriber`, `test_conformance_vectors`, `test_replace_door_latches`. Rust `bindings/rust/src/structured.rs` and TS `vectors.test.mjs` gate only the packed-word codec — neither binding has a graph or latch, so the delivery behaviour is bound by the C++ suite alone. |
| `subscriber/policy-reserved-bits` | Bits 6–15 are **ignored, never rejected**; they do not leak into `reliability`/`priority`; and the record round-trips through `:subscribers[N]` verbatim. | C++ `qos_policy_test.cpp` — `test_policy_reserved_bits_are_ignored`, `test_conformance_vectors`. Rust `structured.rs` tests. TS `vectors.test.mjs`. |
| `settings/removed-knob` | A write of any withdrawn `:settings.<knob>` answers `ERROR{tr::schema::not_found}`, caller-independently, and these are the bytes the resolver builds. | C++ `qos_policy_test.cpp` — `test_removed_knobs_are_schema_not_found`, `test_removed_knob_reply_bytes`; `core/tests/acl_test.cpp` — `test_flat_knob_surface_is_withdrawn` (read **and** write, three callers). |
| `settings/read-container-shape` | A bare `:settings` read serves `SETTINGS{ NAME "app", SETTINGS{…} }` — the container survives, the knobs do not. | C++ `qos_policy_test.cpp` — `test_settings_container_keeps_its_shape`, `test_conformance_vectors` (byte-exact against `read_settings`). |
| `settings/schema-enumerates-nothing` | `:schema`'s synthesized protocol part enumerates zero knobs, and the owner part still follows it. | C++ `qos_policy_test.cpp` — `test_schema_enumerates_nothing`, `test_conformance_vectors` (byte-exact against `read_schema`). |
| `stream/history-depth-host-only` | The STREAM ring depth is owner-side (`set_history_depth` trims the ring) and has **no** wire surface: read and write both answer these bytes. | C++ `qos_policy_test.cpp` — `test_history_depth_is_host_only`, `test_removed_knob_reply_bytes`. |
| `field/field-nested` | `:settings.app` is the two-level field that survives §3.B; a second level that is not `app` resolves to nothing. | C++ `qos_policy_test.cpp` — `test_conformance_vectors` (the vector's own bytes fed to `op_resolver_t` as the selector, with the sibling spelling as the ablation). |
| `fwd/fwd-mint-request` | The bound-path mint request is **bit 7 of the `op` byte** and costs zero added **origin** bytes (RFC-0024 §7.1 amendment 1: forwarding hops may grow the forwarded legs with a reverse `PATH_REF`; the origin's frame never changes); the opcode is `op & 0x3F`, so a mint-flagged READ is still routed as a READ. | C++ `core/tests/bound_path_test.cpp` — `test_mint_round_trip`, `test_unmasked_op_byte_still_routes`, `test_conformance_vectors` (the one-byte diff against `fwd/fwd-read`, byte-exact). Rust `bindings/rust/tests/conformance_vectors.rs` — `fwd_mint_request_is_one_bit`. TS `vectors.test.mjs`. |
| `settings/duplicate-key-last-wins` | The plain NAME-field family (#995): the walk is pair-consuming, and on a repeated key the LAST **well-formed** occurrence wins — a wrong-typed occurrence is skipped, never destructive. | C++ `core/tests/config_reader_test.cpp` — `test_shared_vector_pins_the_walk` (plus the inline #927/#928 fixtures). Rust `conformance_vectors.rs` — `settings_duplicate_key_last_wins`. TS has no NAME-field reader (builders only), so its harness gates the codec alone. |
| `spec/desync-stray-value` | A non-`NAME` in a key slot DESYNCHRONIZES the pair stream: the walk stops rather than resyncing, both SPEC fields read absent, and the create answers `INVALID_PATH` with nothing created (#995 witness). | C++ `core/tests/children_test.cpp` — `test_conformance_vectors` (the vector through the `:children[]` door). Rust `conformance_vectors.rs` — `spec_desync_stray_value_reads_nothing`. |
| `subscriber/policy-last-wins` | The QoS SETTINGS walk is the plain family's (#995): a repeated `delivery_policy` reads the LAST well-formed word, and a wrongly-typed occurrence is SKIPPED — neither an error nor a clobber. | C++ `core/tests/qos_policy_test.cpp` — `test_qos_settings_repeat_semantics_are_the_shared_walk`. Rust `conformance_vectors.rs` — `subscriber_policy_last_wins`. |
| `acl/ace-duplicate-key` | The SECURITY-reader family (#995, disposition #906): a repeated ACE key is REJECTED in every tier — last-wins would widen the narrow-then-wide `access_mask` grant. The codec still round-trips the bytes; the refusal is the reader's. | C++ `core/tests/security_acl_test.cpp` — rejection table 8k + the vector-loading 8k-vector case. Rust `conformance_vectors.rs` — `ace_duplicate_key_is_rejected`, `ace_reject_family_structural_refusals`. |
| `fwd/fwd-mint-reply` | A terminus answers a mint request with a one-element `PATH_REF` as the reply's **last** child, and that element addresses the same vertex on the next operation. | C++ `bound_path_test.cpp` — `test_mint_round_trip` (round-trips the minted element back as a `dst`), `test_conformance_vectors` (byte-exact against what the resolver emits). Rust `fwd_mint_reply_carries_the_binding_last`. TS `vectors.test.mjs`. |
| `acl/bound-vs-canonical-allow` | The bound and canonical spellings of one operation serve **byte-identical** `RESULT` bytes to an allowed caller — RFC-0024 §6.3's mandated pair, allow half. | C++ `bound_path_test.cpp` — `test_bound_read_matches_canonical`, `test_conformance_vectors`. Rust `acl_bound_vs_canonical_allow` and TS `vectors.test.mjs` gate the codec's acceptance of a `PATH_REF` in `dst` position; neither binding has a graph, so the equivalence is bound by the C++ suite alone. |
| `acl/bound-vs-canonical-deny` | The same pair's deny half: `ERROR{tr::access::denied}` `0x0050`, byte-identical outcome tails, and **no minted binding on a denial** — a generation match authorizes nothing. | C++ `bound_path_test.cpp` — `test_mint_denied_by_acl` (including a granted caller's binding handed to a denied one), `test_revocation_is_immediate`, `test_conformance_vectors`. Rust `acl_bound_vs_canonical_deny`. TS `vectors.test.mjs`. |
| `acl/denied-caller-undeclared-app-field` | Namespace-governed disclosure, identical on read and write (RFC-0010 §A erratum 2026-08-12, #435): a caller the ACL denies gets `ERROR{tr::access::denied}` for ANY `settings.app.` spelling — declared, undeclared, `ro`, `wo` — because the gate runs before owner-name resolution, while a protocol-owned nonexistent name answers `ERROR{tr::schema::not_found}` caller-independently on both doors. | C++ `core/tests/acl_test.cpp` — `test_denied_caller_disclosure_parity` (both doors, three callers, both namespaces, existent-surface controls); `core/tests/app_fields_test.cpp` — `test_gating` (the admitted-caller half). No Rust/TS row: neither binding has a graph or an ACL evaluator, so both correctly score `ok` on the codec alone. |
| `fwd/fwd-bound-forward` | A bound `dst` with a residual **longer than one element** makes the receiving host a forwarder: it consumes element 0 — bounds, generation, ACL at the dereferenced vertex — and drops on any failure rather than applying the operation locally. | C++ `core/tests/bound_forward_test.cpp` — the vector is routed through a real `fwd_router_t` and each refusal (stale generation, out-of-range index, an element naming no egress, an ACL denial) is pinned by ablation over a three-node loopback chain. Rust `fwd_bound_forward_is_one_hop_from_forwarded`, TS `vectors.test.mjs` gate the codec's carriage of the shape; neither binding has a router. |
| `fwd/fwd-bound-forwarded` | What that hop puts on the wire: the `dst` shrunk by **exactly one element** (the hop's own, consumed and never rewritten) and `src` grown by the inbound mount run — a bound path changes how the forward address is spelled and nothing about the return route. | C++ `bound_forward_test.cpp` — asserted byte-exact against the router's egress for the paired input, plus an equality over every rope split in `fwd_rope_forward_test.cpp`. Rust / TS as above. |
| `tlv-types/point-schema-app` | `read_schema` emits `POINT{ NAME, SETTINGS, NAME "app", SETTINGS }` for a vertex with a declared app table. | C++ `qos_policy_test.cpp` — `test_conformance_vectors` (byte-exact). |
| `spec/create-child` | A creation SPEC's `type` and `name` field VALUES are `NAME` (`0x02`) nodes, never `VALUE` (`0x01`): the terminus matches each `(NAME key, value)` pair on the value's TYPE, so the `VALUE` spelling is skipped, the catalog selector stays empty, and the create is refused with `INVALID_PATH`. | C++ `core/tests/children_test.cpp` — `test_conformance_vectors` (the vector is byte-exact against the C++ emitter **and** written through the real `:children[]` door, creating the vertex it names) and `test_value_typed_spec_is_refused` (the ablation: the same two fields as `VALUE`s ⇒ `INVALID_PATH`, nothing created, with the `NAME` spelling as the positive control). Rust `bindings/rust/tests/conformance_vectors.rs` — `spec_create_child`, `spec_value_typed_fields_are_not_the_vector`. TS `vectors.test.mjs`. |
| `spec/conn-client-ws` | A connection SPEC's `config` mixes value types by key and the two are not interchangeable: `role`/`port` are opaque `VALUE`s, `kind`/`addr` are textual `NAME`s — a string key is found only as a `NAME` child, so a core that can emit only one form cannot express half the record. | C++ `children_test.cpp` — `test_conformance_vectors` reads the vector's `config` back through the production `tr::net::config_reader_t` (`u8`/`u16` for `role`/`port`, `name` for `kind`/`addr`) at the child factory the create actually reaches. Rust `spec_conn_client_ws` (byte pin on `settings_typed` + `spec`). TS `vectors.test.mjs` — `encodeConnSpec` byte-pin; `conn-spec.test.mjs` pins the same bytes against the captured C++ emitter output. |
| `fwd/fwd-routed-two-mount` | A `dst` crossing TWO `net/<module>/<name>` mounts is consumed by two different routers — `strip_k = 3` at each — and only the residual `/sensor/temp` resolves at the third node; `src` grows by each forwarder's FULL mount run. | C++ `core/tests/fwd_two_mount_test.cpp` — three in-process `fwd_router_t` nodes chained by loopback link pairs, wired through the production `transport_vertex_t` `:children[]` SPEC door. No binding gates it: neither the Rust nor the TypeScript core has a router, so both correctly score `ok` on the codec alone. |
| `fwd/fwd-src-accumulated` | The reverse half of the same invariant, seen mid-route: after two hops `src` has grown by **two full** `net/<module>/<name>` mount runs (six segments, most recent first), never by two bare segments, while `dst` still carries the next mount plus its residual. | C++ `core/tests/fwd_two_mount_test.cpp` — the hop-2 probe asserts `src == /net/downlink/a/net/downlink/cli/reply-ep` byte-exactly, the identical two-run shape this vector carries. Same binding caveat as the row above: no Rust/TS router. |
| `fwd/fwd-reply-result` | The closing half of the round trip: the terminus emits its reply with the request's routes **swapped verbatim** — reply `dst` = the accumulated request `src` (so it is routable home by the same strip-K descent), reply `src` = the request `dst`. | C++ `core/tests/fwd_two_mount_test.cpp` — the hop-2 probe pins the request `src` this vector's `dst` mirrors byte-exactly, and the terminus leg asserts the REPLY reaches the originator with `dst` fully consumed to `/reply-ep`; `core/tests/op_resolve_test.cpp` drives the swap itself. Same binding caveat: no Rust/TS router. |
| `fwd/fwd-reply-error` | The same terminus, the same swapped routes, on the error side — the reply `src` is the **refused spelling** (the request `dst` that did not resolve), and the payload is `STATUS{ ERROR{ VALUE u16 } }` per RFC-0002 §C. | C++ `core/tests/op_resolve_test.cpp` — a non-local `dst` answers `kind=ERROR` with `STATUS{ ERROR{ VALUE u16=0x0020 tr::path::not_found } }`, byte-exact on the code. Rust `bindings/rust/tests/conformance_vectors.rs` and TS `vectors.test.mjs` gate the reply codec and the error-code read, not the routing. |
| `path/path-reserved-brackets` | The segment reserved-character set is exactly `/ : . [ ] * ?` (reference/03 §Reserved characters, normative via spec v1 §3): the vector's `frame[7]` segment is codec-legal bytes every core must carry bit-for-bit, and a segment every tier's ONE predicate (ADR-0073 §1) must refuse with `tr::path::invalid` — the #996 closure, where C++ admitted the brackets Rust/TS refused. | C++ `core/tests/path_test.cpp` — the vector's segment payloads fed to `tr::graph::valid_segment` (control `camera` passes, `frame[7]` fails) plus the per-character sweep. Rust `bindings/rust/tests/conformance_vectors.rs` — `path_reserved_brackets` (same pair over `validate_segment`, same sweep). TS `bindings/typescript/packages/client/test/vectors.test.mjs` — same pair over `encodePath` / `RESERVED_SEGMENT_CHARS`, same sweep. Relaxing any one tier's set reddens that tier's own suite. |
| `fwd/fwd-reply-error-after-description` | The **reader's** half of the row above, and the one place the acceptance rule is written down: a reply's ERROR is the **first `ERROR` child of the STATUS, at whatever position** — reference/05 §`0x09` pins no order over a STATUS's children, and RFC-0002 §C pins position only INSIDE the ERROR ("its first child is the identity"). Emitters stay canonical (ERROR first, as the twin vector); this is what a reader must **accept**, not a licence to emit. | Rust `bindings/rust/tests/conformance_vectors.rs` — `fwd_reply_error_after_description`. TS `vectors.test.mjs` — `replyErrorCode reads the ERROR at any STATUS child position`. Both assert the STATUS's child 0 is the DESCRIPTION *before* asserting the read, so the vector cannot be re-blessed into the easy case and keep passing. No C++ row: the C++ core emits this shape's twin and has no production reply-side reader (its `children[0]` uses are byte assertions on its own emitter's output). The divergence this pins (#878) was TS demanding `children[0]` where Rust scanned; ablating **either** core to the other's rule reddens its own test with the same `0 != 32`. |
| `path-label/label-roundtrip` | An RFC-0027 path label is **one 7-byte escape record** at `kind = 0x16` inside the packed `PATH` body — `00 16 04 <u32 LE>` — and not the 8-byte `PATH_LABEL` TLV child §5.3 proposed (amendment 5). Generation `0` is reserved and never reaches the wire. | C++ `core/tests/path_label_test.cpp` — `element_grammar` (emit → `path_label_at` round trip, the refusal of a zero generation) and `conformance_vectors` (the vector built from `emit_path_label`, byte-exact). No Rust/TS row: neither binding has a label emitter yet (#1325 car 7), so both correctly score `ok` on the codec alone. |
| `path-label/label-mixed` | RFC-0027 §5.2: name and label elements mix **in any order**, with no "fully minted" state — a hop that does not mint leaves its own part a string while every other hop's part still compacts. A label is found by **walking** and reading kinds, never by counting positions. | C++ `path_label_test.cpp` — `element_grammar` (a four-element body walked with `p += packed_record_span`, yielding exactly its two labels) and `conformance_vectors`. |
| `path-label/label-multi-segment` | RFC-0027 amendment 6: one label covers a hop's **whole local part**, so the element is the same 7 bytes whether the mount run is one segment or three — nothing on the wire encodes the run's width. | C++ `path_label_test.cpp` — `conformance_vectors` (the 15-byte packed run against the 7-byte element, byte-exact against the emitter). |
| `path-label/label-wrong-length` | A label payload that is not exactly 4 bytes is a **malformed address**: a hop that implements `0x16` reads nothing from it and refuses (`tr::path::invalid`), while a hop that does not still steps over it by its declared length. | C++ `path_label_test.cpp` — `element_grammar` + `conformance_vectors` (`path_label_at` answers nothing, `packed_record_span` still skips it, and the body is not a key). The resolver's `tr::path::invalid` answer for an escape-carrying `dst` is bound by `core/tests/op_resolve_test.cpp` and `core/tests/fwd_terminus_reject_test.cpp`. |
| `path-label/label-foreign-kind` | An element is read by its **kind**, never by its position: an escape at an unassigned kind is skipped by length and MUST NOT be dereferenced as a label — the mis-delivery a length-only check would cause. | C++ `path_label_test.cpp` — `element_grammar` + `conformance_vectors` (a foreign kind carrying exactly four bytes still answers "not a label"). Ablating the kind check reddens it while `label-wrong-length` still passes, which is why the two are separate vectors. |
| `fwd/fwd-label-mint-reply` | RFC-0027 §6.1: a MINTING forwarder relays a reply with its own local part — the whole three-segment mount run, 13 packed bytes — spelled as one 7-byte path-label element; a hop with no injected table relays the reply byte-identically and is fully conformant (§6.3). It also pins §6.2's trigger: the mint is a subscription's first fire and no other condition, so every later reply reuses the same label and the table never grows. | C++ `core/tests/path_label_forward_test.cpp` — the vector is asserted byte-exact against what a real `fwd_router_t` puts on the wire, with an un-injected hop as the control and the four-replies-one-label case pinning the trigger. No Rust/TS row: neither binding has a router **or** a label emitter, so both correctly score `ok` on the codec alone — and none is owed. A binding is a codec; its whole obligation to RFC-0027 is to carry the escape record in a frame path and refuse it in canonical/key context, which both already do (`tlv_builders::packed_segments`, `pathSegments`). The label is decoded for a *human* in the [Wireshark dissector](https://github.com/avatarsd-llc/libtracer/tree/main/tools/wireshark), which renders it as `<label:index@generation>` and pins that against these vectors' own bytes. |
| `fwd/fwd-label-stale` | RFC-0027 §7.1/§7.2: the identical bytes forward against a live slot and answer `tr::path::not_found` (`0x0020`) against a bumped generation — staleness is table state, not wire state. On a refusal the hop forwards nothing, applies nothing, attempts **no repair of any kind**, and does **not** fall through to the canonical walk (the label replaced the string bytes, so there is nothing left to walk); the refusal is counted, and it mutates no table state. | C++ `path_label_forward_test.cpp` — the departure bump via `remove_child`, the counted refusal, the error identity read out of `FWD > STATUS > ERROR > VALUE`, a second identical frame refusing identically, and a label this host never minted taking the same answer. Same binding caveat as the row above. |
| `fwd/fwd-label-terminus-reply` | RFC-0027 §6.1 **point 3**: a MINTING TERMINUS answers with the residual it resolved — `/sensor/temp`, 12 packed bytes — spelled as one 7-byte path-label element, so the reply's whole `src` IS the label and the frame gets shorter. Point 3 is what makes point 4's *fully-minted `src`* reachable at all: the forwarding hops' labels stack in front of this one. A terminus with no injected table echoes the request's `dst` unchanged and is fully conformant (§6.3), and a **denied** operation mints nothing (§8.1). | C++ `core/tests/path_label_terminus_test.cpp` — the vector is asserted byte-exact against what a real `fwd_router_t` + `op_resolver_t` puts on the wire, with an un-injected node as the control, an ACL-denied read pinning §8.1, a mint-flagged request pinning §11.2, and the five-replies-one-label case pinning §6.2's trigger. Same binding caveat as the two rows above. |
| `fwd/fwd-label-terminus-deref` | RFC-0027 §7.2 at a TERMINUS: the label of the row above, **presented back**. The whole `dst` is one 7-byte element and there is no name beside it, so 33 bytes carry what 38 carried — the saving §12.4 axis 2 measures. Whether a label names a link (a hop) or a local vertex (a terminus) is a fact about the minting host's TABLE and not about these bytes. §8.2 still governs: the ACL is evaluated at the dereferenced vertex for the operation's own right, and a generation match authorizes nothing; §11.2 keeps a `PATH_REF` mint off the answer. | C++ `core/tests/path_label_terminus_test.cpp` — the round trip against the production wiring (the reply is byte-identical to the string spelling's), the allow/deny pair over one gated vertex, the mint-flagged labelled read acquiring no `PATH_REF`, and the deref spending no second slot. Same binding caveat as the rows above. |
| `fwd/fwd-label-terminus-stale` | RFC-0027 §7.2's refusal at a terminus: `ERROR{tr::path::not_found}` `0x0020`, with the refused label echoed in the reply's `src` because a refusing terminus rewrites nothing. One answer covers out-of-range, generation mismatch, an unminted slot, a foreign peer's label, a re-added child's re-stamped identity and a vertex that retired after the mint. Nothing is forwarded, applied, repaired or minted, and no table state moves. | C++ `core/tests/path_label_terminus_test.cpp` — byte-exact against what the node emits, plus the counter moving, the carried WRITE landing nowhere, and the re-add case pinning §7.1's second stamp. Same binding caveat as the rows above. |
| `acl/label-vs-string-allow` | RFC-0027 §8.2's mandated pair, allow half: the labelled and string spellings of one operation forward **byte-identical** residuals to an allowed caller. A generation match authorizes nothing — the ACL is evaluated at the dereferenced vertex for the operation's own right, through the same `bound_egress` the bound form runs. | C++ `path_label_forward_test.cpp` §8 — both spellings driven through the production `fwd_router_t` and their egress compared, with the vector byte-pinned against the labelled operation. No Rust/TS row: neither binding has a graph or an ACL evaluator. |
| `acl/label-vs-string-deny` | The same pair's deny half: `ERROR{tr::path::not_found}` `0x0020` — **not** `tr::access::denied`, deliberately, because §8.1's anti-enumeration property requires a denied label and a stale one to be indistinguishable (`denied` would confirm the slot is live). The reply's `src` echoes the refused spelling so the sender can correlate it and fall back to strings. | C++ `path_label_forward_test.cpp` §8 — the refusal byte-pinned against the vector, plus the asymmetry this row states honestly: the label arm gates at the dereferenced vertex where the canonical mount descent gates nowhere (a name-addressed operation is gated at the TERMINUS), so the assertion is that the labelled spelling is **never more permissive** than the string one rather than a bare equality. |
`tlv-types/settings-reliability` is **not** in this table on purpose: since RFC-0022 it pins
a `SETTINGS` record *shape* and no longer names any protocol knob, so it is a pure codec
vector with no behaviour to bind.

When a core does not implement a vector's behaviour, the honest state is *not* a silent
`ok` on the codec score plus an empty row here — it is either a binding test in that core's
suite, or an explicit note in [`harnesses.json`](harnesses.json) that the surface is absent.

## What a harness must do

A harness is a single command:

```
<harness> <vectors-dir>
```

For **every** case directory under `<vectors-dir>` that contains an `input.bin`, it
MUST check, at minimum, the **round-trip**:

> `encode(decode(input.bin)) == input.bin`   (byte-for-byte)

A harness SHOULD additionally check the **semantic** form (`decode(input.bin)`
matches `expected.json`) where the language has a JSON parser to hand.

A decode that fails, or a round-trip that differs by one byte, is a `not ok`.

For every case directory that contains a `reject.bin` (negative case), it MUST
check instead that

> `decode(reject.bin)` **fails**, with the error named by `expected.json`'s
> `"reject"` field

A decode that *succeeds*, or that fails with a different error, is a `not ok`.
Both kinds of case are folded into the same sorted TAP list, keyed by directory.

## Output: TAP version 13

The harness writes [TAP](https://testanything.org/) to **stdout**, one line per
vector, keyed by the vector's path **relative to `<vectors-dir>`** (so keys match
across cores):

```
TAP version 13
1..3
ok 1 - crc/value-crc32c
ok 2 - framing/empty-status-ok
not ok 3 - path/path-sensor-temp
```

- The `1..N` plan line states how many vectors were run.
- The description after `-` is the vector's relative path (no leading `./`), forward
  slashes on every platform.
- Exit code: `0` if every vector is `ok`, non-zero otherwise.
- Diagnostics may be written to **stderr** (ignored by the driver) or as TAP
  `# comment` lines.

## Registering the harness

Add an entry to [`harnesses.json`](harnesses.json). The driver
[`run-all.py`](run-all.py) appends `<vectors-dir>` to your `cmd`, runs it, parses the
TAP, and folds it into the cross-core matrix. Set `"enabled": false` while a core is
still a stub — it then shows as *pending* and does not gate.

## Reference harnesses

- **C++** (golden): `core/tests/conformance_runner --tap <vectors-dir>`.
- **TypeScript** (pure-TS core): `node bindings/typescript/packages/core/conformance/harness.mjs <vectors-dir>`.
- **Rust** (native `no_std` + alloc core): `cargo run --manifest-path bindings/rust/Cargo.toml --example conformance -- <vectors-dir>`.

All three are registered and `enabled` in [`harnesses.json`](harnesses.json), so the driver gates on their cross-core agreement.

## Differential fuzzing (cross-core drift, beyond the curated vectors)

The curated vectors pin a set of hand-picked wire shapes. To catch drift those
points *miss*, [`diff_fuzz.py`](diff_fuzz.py) generates random **valid** frames
from an explicit seed and round-trips each through **all three** cores, asserting
byte-for-byte agreement (ADR-0028 / ADR-0032 extended from curated points to a
fuzzed corpus). A mismatch is a genuine cross-core bug; the failing seed + bytes
are printed so the frame can be promoted to a curated vector.

```
# build the C++ core first (or set $LIBTRACER_CXX_HARNESS), then:
python3 tests/conformance/diff_fuzz.py            # 1000 seeds (default)
python3 tests/conformance/diff_fuzz.py --seeds 5000
python3 tests/conformance/diff_fuzz.py --start 4242 --seeds 1   # reproduce one seed
```

It drives a small **`--roundtrip`** batch hook each harness implements alongside
`--tap`: read one hex frame per stdin line, print the `decode`→`encode`
re-encoding as hex (or `ERR:<reason>` on a decode failure), one line per input.
A new core joins the differential gate by implementing that hook.
