# Wire audit: every element, keep, rename, merge or delete

> **Status: informative, tracked.** This page changes nothing on the wire. It is the audit
> [#1970](https://github.com/avatarsd-llc/libtracer/issues/1970) asks for before the Tracer v1
> freeze ([#1961](https://github.com/avatarsd-llc/libtracer/issues/1961)). Each delete,
> rename or merge lands later as its own erratum or amendment, through
> [GOVERNANCE.md](https://github.com/avatarsd-llc/libtracer/blob/main/.github/GOVERNANCE.md).
> Where this page and the specification disagree, the specification wins.

The elements come from [Protocol v1](../spec/v1.md) and the three annexes its §3 incorporates:
[01-data-format.md](../reference/01-data-format.md),
[05-protocol-tlvs.md](../reference/05-protocol-tlvs.md) and
[03-addressing.md](../reference/03-addressing.md) §path syntax. Each one was cross-checked
against the C++ codec and resolver and against `tests/conformance/vectors/v1/`. The audit was
taken at main `4a7496aa`.

## How to read the tables

| Column | Meaning |
| --- | --- |
| Element | The name the spec gives it. |
| Codepoint | Its value on the wire: a type code, a bit, a code, or a key string. |
| Status | What the spec says now: *assigned*, *reserved*, *retired*, *unread*, … |
| Shipped use | Evidence on main: a symbol in the reference core, a conformance vector, or *none found*. |
| Stage | The [#1938](https://github.com/avatarsd-llc/libtracer/issues/1938) stage that removes or changes it, or another accepted instrument already doing so. |
| Verdict | **keep**, **rename**, **merge** or **delete**, with a one-line reason. |
| F | The proposed follow-up (§Follow-ups) for a verdict nothing else covers yet. |

"Covered" means an accepted RFC or an open ticket already removes the element, so no new issue
is needed. Vectors are named by `<category>/<case>`.

## Verdict counts

| Verdict | Rows | Already covered | Needs a follow-up |
| --- | ---: | ---: | ---: |
| keep | 122 | — | — |
| rename | 2 | 0 | 2 |
| merge | 2 | 0 | 2 |
| delete | 37 | 16 | 20 (+1 already done) |
| **total** | **163** | | |

The 24 verdicts that need a new instrument fold into 12 proposed follow-ups (F1–F12).

## 1. Frame header and `opt` bits (01-data-format)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| `type` | byte 0, u8 | assigned | every frame; `core/include/libtracer/tlv.hpp:type_t` | — | **keep**: the dispatch byte. | |
| `opt` | byte 1, u8 | assigned | every frame | — | **keep** | |
| `length` | u16 LE (`LL=0`) / u32 LE (`LL=1`) | assigned | every frame; `tlv-types/value-ll-u32` | — | **keep**: fixed width, no LEB128. | |
| `opt` bit 7 R | `0x80` | reserved, reject | `framing/reserved-bit7-reject` | — | **keep**: frozen for v1; the reject stops silent drift. | |
| `opt.PL` | `0x40` | assigned | `framing/pl-bit-opaque`, `framing/pl-bit-structured` | — | **keep** | |
| `opt.TS` + `trailer_ts` | `0x20` | assigned | reference writer stamps TF=0 and the terminus echoes it; `tlv-types/value-ts-abs` | — | **keep**: the RTT echo needs it. | |
| `opt.CR` + `trailer_crc` | `0x10` | assigned | `crc/value-crc32c`; a root CRC failure is dropped by `fwd_router.cpp` | — | **keep** | |
| `opt.LL` | `0x08` | assigned | `tlv-types/value-ll-u32` | — | **keep** | |
| `opt.CW` (CRC-16-CCITT) | `0x04` | assigned | `crc/value-crc16`, `crc/value-rel-ts-crc16-nested` | — | **keep**: the small-frame CRC for narrow links. | |
| `opt.TF` (relative i32 TS) | `0x02` | reserved grammar: decodable, relayable, no writer | `framing/relative-ts-nested`, `stream/tf1-reserved` | — | **keep**: ruled 2026-08-20 as additive future surface (RFC-0025 §4.2.1 Am. 1). | |
| `opt` bit 0 R | `0x01` | reserved, reject | `framing/reserved-bit0-reject` | — | **keep** | |

## 2. Type-code registry (05-protocol-tlvs)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| sentinel | `0x00` | never valid | rejected as `tr::frame::invalid` | — | **keep** | |
| VALUE | `0x01` | assigned | everywhere; `tlv-types/value-bool-true` | — | **keep** | |
| NAME | `0x02` | assigned | SETTINGS keys, `:schema` labels, FIELD levels | — | **keep** | |
| DESCRIPTION | `0x03` | assigned | ERROR and STATUS human detail; the Rust `error_registry` reads and emits it; `errors/error-registered-detail`, `fwd/fwd-reply-error-after-description` | — | **keep**: the one human-text carrier. | |
| SUBSCRIBER | `0x04` | assigned | `core/src/graph_fields.cpp:parse_subscriber_tlv`; `tlv-types/subscriber-path` | — | **keep** | |
| reserved | `0x05` | reserved, never assigned | none (by design) | — | **keep** as reserved: it marks the retired generic container. | |
| PATH | `0x06` | assigned | every `dst`/`src`; `path/path-sensor-temp` | 1 (#1939 walks it) | **keep** | |
| POINT | `0x07` | assigned | composed branch read, `:schema`, `:subscribers[]` read | — | **keep** | |
| ERROR | `0x08` | assigned | `kind=ERROR` replies; `errors/*` | 4 (reply becomes a write) | **keep**: the error carrier outlives REPLY. | |
| STATUS | `0x09` | assigned | reply wrapper and slot-clear sentinel; `framing/empty-status-ok` | — | **keep** | |
| ACL | `0x0A` | assigned | `:acl`; `acl/acl-aces` | — | **keep** | |
| SETTINGS | `0x0B` | assigned | qos, `:settings`, `:identity`, `:stats`; `settings/*` | — | **keep** | |
| TIME | `0x0C` | assigned, core-neutral | the batch base child written by `tr::wire::compose_batch`; `stream/batch-time-roundtrip` | — | **keep**: the batch layout needs it. | |
| ROUTER | `0x0D` | reserved, decodable, MUST NOT emit | none emitted; only the stale `tlv-types/router-wrapped` vector, whose helpers were deleted | — | **delete**: retire it like `0x14`; the flooding profile it waits for has no plan. | F11 |
| SPEC | `0x0E` | assigned | creator endpoint and `:children[]`; `conn/create-via-spec`, `spec/create-child` | — | **keep** | |
| FWD | `0x0F` | assigned | every remote op; `fwd/*` | 4 (ops change) | **keep** | |
| FIELD | `0x10` | assigned | `field/*` | — | **keep** | |
| ADVERTISE | `0x11` | assigned, no vectors | `fwd_router.cpp` (route-handle plane) | 6 (#1950, #1951) | **delete**: COMPACT goes. | covered |
| COMPACT | `0x12` | assigned, no vectors | `fwd_router.cpp` | 6 (#1950, #1951) | **delete** | covered |
| HANDLE_NACK | `0x13` | assigned, no vectors | `fwd_router.cpp` | 6 (#1950, #1951) | **delete** | covered |
| PATH_REF (retired) | `0x14` | retired by RFC-0029 §5.3; MUST be refused as `dst` | still emitted and honoured on main; `path-ref/ref-*`, `fwd/fwd-bound-*` | RFC-0029 S1–S2 (PR #1684 (RFC-0029 S1: the PAIR element and its hop arm)) | **delete**: the PAIR element replaces it; the code stays reserved, never reassigned. | covered |
| PATH_REF_REVERSE | `0x15` | assigned | `op_resolve_walk.hpp` reads it; `fwd/fwd-reverse-mint` | RFC-0029 S5 re-spells the body | **keep** | |
| unassigned | `0x16`–`0x1F` | unassigned | none | — | **keep** | |
| long-term registry | `0x20`–`0x7F` | reserved | none | — | **keep** | |
| BATCH | `0x80` | assigned in the user range | `core/include/libtracer/batch.hpp`; `stream/batch-composed-standalone` | — | **keep** | |
| user range | `0x81`–`0xFF` | no protocol opinion | none | — | **keep** | |

## 3. `PATH` body records (05 §`0x06`, 03 §path syntax)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| segment record | `[u8 len 1..64][utf8]` | assigned | `path/path-sensor-temp`, `path/path-deep-255-packed` | — | **keep** | |
| escape record | `00 <kind> <len> <bytes>` | assigned | `path/path-escape-in-key-context`, `path-label/label-foreign-kind` | — | **keep**: the self-delimiting extension point. | |
| PAIR element | escape `kind=0x16`, `len=8` | normative (RFC-0029) | none on main; PR #1684 (RFC-0029 S1: the PAIR element and its hop arm) is open | 1 (#1939) | **keep**: the one compact address. | |
| path label | escape `kind=0x16`, `len=4` | retired by RFC-0029 §5.3 | `path_element_kind_t::LABEL`; `path-label/*`, `fwd/fwd-label-*` | RFC-0029 S1–S3 | **delete** | covered |
| `0x15` bare 8-byte array body | RFC-0024 form | retired by RFC-0029 §7.1 | `path-ref/reverse-len-not-multiple-of-8` | RFC-0029 S5 | **delete**: the body becomes a `PATH` body. | covered |
| empty `PATH` | length 0 | assigned | root `/`, the unacknowledged `src`, the re-headed `dst` | — | **keep** | |
| segment length cap | 64 B | assigned | `core/src/path.cpp` | — | **keep** | |
| total path cap | 1024 B of encoded body | assigned | `path/path-deep-204` | — | **keep** | |
| segment count cap | 255 | assigned | `path/path-deep-255-packed` | — | **keep** | |
| reserved characters | `/ : . [ ] * ?` | assigned | `path/path-reserved-brackets` | — | **keep** | |
| address-segment index | `segment = name [index]` | grammar only, no wire carrier | none found | — | **delete**: an ABNF production with no encoding. | F9 |
| path as a VALUE string "where a path is expected" | VALUE `0x01` | MUST accept (05 §string form) | none: `parse_subscriber_tlv` reads only a PATH child | — | **merge** into the PATH form: the text contradicts what ships. | F10, done ([#2014](https://github.com/avatarsd-llc/libtracer/pull/2014)) |

## 4. `SUBSCRIBER` and its `qos_settings` (05 §`0x04`)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| `target_path` | PATH child | required | `parse_subscriber_tlv`; `tlv-types/subscriber-path` | — | **keep** | |
| `qos_settings` | SETTINGS child | optional | `parse_subscriber_tlv`; `subscriber/*` | — | **keep** | |
| `capability` | ACL child | optional | none found: no reader | — | **delete**: subscribe is gated by the producer's `:acl`, not a carried token. | F1 |
| `subscriber_id` | NAME child | optional | none found: no reader | — | **delete** | F1 |
| no-`target_path` "clear this slot" | SUBSCRIBER without PATH | MUST (05 §Validation) | the field door refuses it (`TYPE_MISMATCH`); the wire door subscribes | — | **delete**: the empty STATUS is the shipped sentinel. | F2 |
| empty STATUS to `:subscribers[N]` | `09 00 00 00` | assigned | `graph_fields.cpp` slot-clear arm | — | **keep** | |
| `delivery_scope` | key | reserved, read by nothing | none found | — | **delete** | F3 |
| `delivery_compact` | key, u8 | assigned | `parse_subscriber_tlv`; `subscriber/policy-absent` | 6 (#1950, #1951) | **delete** | covered |
| `delivery_policy` | key, u16 | assigned | `parse_subscriber_tlv`; `subscriber/policy-*` | — | **keep** | |
| `batch_count` | key, u32 | retired (RFC-0025 Am. 4) | none: carried, read by nothing | — | **delete** | F3 |
| `batch_window_ns` | key, u64 | retired (RFC-0025 Am. 4) | none: carried, read by nothing | — | **delete** | F3 |
| `reliability` | policy bits 0–1 | stored, "awaits nothing" (RFC-0025 erratum 2026-08-24) | stored and read back only | — | **delete**: return the bits to reserved. | F3 |
| `priority` | policy bits 2–4 | stored, not yet honoured | stored and read back | — | **keep**: the one egress hint; honouring it is transport work. | |
| `durability_request` | policy bit 5 | honoured | the latch; `subscriber/policy-durability` | — | **keep** | |
| `delivery_class` | policy bits 6–7 | decoded, not yet honoured | all three cores decode it; `subscriber/policy-reserved-bits` | — | **keep**: phase 3 of #1204 (delivery classes) honours it. | |
| reserved policy bits | bits 8–15 | MUST write 0, MUST ignore | `subscriber/policy-reserved-bits` | — | **keep** | |
| per-vertex `delivery_mode` | `:settings` key | "deferred" wire spelling | none: the core namespace is empty | — | **delete**: host-only state; the text contradicts RFC-0022. Done: erratum, PR [#2013](https://github.com/avatarsd-llc/libtracer/pull/2013). | F4 |

## 5. `POINT`, `ERROR`, `STATUS` (05 §`0x07`–`0x09`)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| POINT `vertex_name` | NAME, first child | required | branch read and write, `:schema` | — | **keep** | |
| POINT `value` | VALUE | optional | composed branch read, branch write | — | **keep** | |
| POINT `description` | DESCRIPTION | optional | none found: nothing emits it, a branch write refuses it | — | **delete** | F5 |
| POINT `default_settings` | SETTINGS | optional | the `:schema` protocol part; `settings/schema-enumerates-nothing` | — | **keep** | |
| POINT SUBSCRIBER children | SUBSCRIBER | optional | the `:subscribers[]` read (RFC-0004 §D) | — | **keep** | |
| POINT child POINTs | POINT | optional | branch read and write | — | **keep** | |
| `:schema` owner part | NAME `"app"`, `"access"` | assigned | `tlv-types/point-schema-app` | — | **keep** | |
| ERROR registered identity | first child VALUE u16 | assigned | `core/src/fwd_reply.cpp:error_code`; `errors/error-registered-code` | — | **keep** | |
| ERROR string identity | first child NAME | assigned | `errors/error-string-form` | — | **keep**: third-party codes without an RFC. | |
| ERROR detail children | DESCRIPTION / VALUE | optional | `errors/error-registered-detail` | — | **keep** | |
| bare ERROR as reply payload | ERROR without STATUS | removed by the RFC-0004 erratum 2026-10-08 (was MAY, 05 §Where it appears) | none: the core always wraps, and its reader skips a bare ERROR; `fwd/reply-error-bare-ignored` | — | **merge** into `STATUS{ERROR}`: two spellings of one answer. | F6: done, #1983 (one error-reply spelling), PR #2017 |
| STATUS empty (OK) | `09 00 00 00` | assigned | `framing/empty-status-ok` | — | **keep** | |
| STATUS non-empty | `PL=1`, ERROR + DESCRIPTION | assigned | every error reply | — | **keep** | |

## 6. Error registry (05 §`0x08`)

Severity and disposition are registry metadata and never travel on the wire, so they are not
rows here.

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| `tr::frame::truncated` | `0x0001` | assigned | decode verdict in the C++ API (`frame.hpp`); not sent: an undecodable frame has no route | — | **keep**: one identity for the API and any peer that does answer. | |
| `tr::frame::invalid` | `0x0002` | assigned | decode verdict; `errors/error-invalid-frame` | — | **keep** | |
| `tr::frame::crc_fail` | `0x0003` | assigned | decode verdict; the router drops the frame | — | **keep** | |
| `tr::tlv::nesting_too_deep` | `0x0010` | assigned | decode verdict (`frame.cpp`) | — | **keep** | |
| `tr::path::not_found` | `0x0020` | assigned | emitted; `errors/error-registered-code` | — | **keep**: every PAIR refusal arm answers it. | |
| `tr::path::invalid` | `0x0021` | assigned | emitted | — | **keep** | |
| `tr::path::in_use` | `0x0022` | assigned | emitted; `conn/spec-name-in-use` | — | **keep** | |
| `tr::schema::type_mismatch` | `0x0030` | assigned | emitted; `errors/error-kindless-spec-type-mismatch` | — | **keep** | |
| `tr::schema::not_found` | `0x0031` | assigned | emitted; `settings/removed-knob` | — | **keep** | |
| `tr::flow::backpressure` | `0x0040` | assigned | emitted (a refused source) | — | **keep**: receiver-pays refusals need it. | |
| `tr::flow::timeout` | `0x0041` | assigned | emitted by the terminus AWAIT wait; `errors/error-registered-detail` | 4 (#1946 retires AWAIT) | **delete**: after stage 4 the deadline is the app's and no node sends it. | F7 |
| `tr::flow::address_shift_gap` | `0x0042` | assigned | never sent; a host-side gap count (`graph_t::stream_gaps`) | — | **rename** to `tr::flow::gap`: the spec already calls the name historical. | F7 |
| `tr::access::denied` | `0x0050` | assigned | emitted | — | **keep** | |
| `tr::transport::down` | `0x0060` | assigned | emitted (`transport_can.cpp`) | — | **keep** | |
| `tr::version::mismatch` | `0x0070` | assigned | none found: no emitter, no in-band version | — | **delete**: discovery is out of v1's scope, so nothing on the wire can raise it. | F7 |

## 7. `ACL` (05 §`0x0A`)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| ACE `type` | key, u8: ALLOW=0, DENY=1 | assigned | ALLOW in the core subset; DENY in `full_acl_policy_t`; `acl/acl-aces` | — | **keep** | |
| ACE `flags` INHERIT | `0x1` | assigned | evaluated by both policies | — | **keep** | |
| ACE `flags` INHERIT_ONLY | `0x2` | assigned | none: both policies refuse it at write | — | **delete** | F8 |
| ACE `flags` NO_PROPAGATE | `0x4` | assigned | none: refused at write | — | **delete** | F8 |
| ACE `flags` GROUP | `0x8` | assigned | none: refused at write | — | **delete** | F8 |
| ACE `subject` | key | assigned | `acl/acl-aces` | — | **keep** | |
| subject `EVERYONE@` | string | the one special subject | `security_acl.hpp` | — | **keep** | |
| ACE `access_mask` | key, u32 (RFC-0026) | assigned | `core/include/libtracer/acl_ace.hpp:acl_right_t` | — | **keep** | |
| right READ | `0x01` | assigned | reads, `await` | — | **keep** | |
| right WRITE | `0x02` | assigned | writes, delivery gate, connection removal | — | **keep** | |
| right SUBSCRIBE | `0x04` | assigned | the `:subscribers[]` append | — | **keep** | |
| right CREATE | `0x08` | assigned | write-creates, creator endpoint | 4 (#1945 makes creation opt-in) | **keep**: still the gate on an opted-in parent. | |
| right DELETE | `0x10` | assigned, "no core surface yet" | none: removal takes WRITE (RFC-0009 §A.2) | — | **delete** | F8 |
| right READ_ACL | `0x20` | assigned | `:acl` read | — | **keep** | |
| right WRITE_ACL | `0x40` | assigned | `:acl` write (the admin right) | — | **keep** | |
| right WRITE_OWNER | `0x80` | assigned, "no core surface yet" | none: no owner identity exists | — | **delete** | F8 |
| reserved rights | `0x100`+ | reserved | none | — | **keep** | |
| ACE `expires_ns` | key, u64 | optional | evaluated against the core's own clock (`graph.cpp`) | — | **keep** on the wire; the clock moves to an app seam (#1792 (findings: dropping await, existing timers)). | |

## 8. `SETTINGS` surfaces (05 §`0x0B`)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| `app` key | `"app"` | reserved for owner fields | `settings/read-container-shape` | — | **keep** | |
| module-namespaced nested SETTINGS | `<module>` key | grammar; arm (a) unimplemented | none | — | **keep**: the namespacing rule for future module fields. | |
| removed vertex knobs (7 names) | `reliability` … `store_ref_min_bytes` | removed (RFC-0022) | answer `not_found`; `settings/removed-knob` | — | **delete**: already done. | done |
| `:identity` `kind` | key, u8 | assigned | `graph_fields.cpp` | — | **keep** | |
| `:identity` `key` | key, bytes | assigned | `graph_fields.cpp` | — | **keep** | |
| identity kind reserved | `0x00` | reserved, invalid | rejected | — | **keep** | |
| identity kind ed25519 | `0x01` | assigned | the only kind | — | **keep** | |
| `:stats.mem.control` | seam | assigned | `settings/stats-seam-block` family | — | **keep** | |
| `:stats.mem.ring` | seam | assigned | the graph-level ring source census | — | **keep** | |
| `:stats.graph.delivery` | seam | assigned | `settings/stats-seam-block` | — | **keep** | |
| `:stats.router.drops` | seam | assigned | `settings/stats-seam-net-router` | 3, 6 may drop nouns | **keep**: readers ignore unknown nouns. | |
| `:stats.labels.table` | seam | assigned, deleted by RFC-0029 S3 | label table on main | RFC-0029 S3 | **delete** | covered |
| `:stats.link.<child>` | seam | assigned | per-link counters | — | **keep** | |
| `labels_used` noun | `:stats.link.<child>` noun | assigned, deleted by RFC-0029 S3 | label table on main | RFC-0029 S3 | **delete** | covered |

## 9. `FIELD` and the field namespace (03 §index forms, RFC-0004 §C)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| level `field_name` | NAME | assigned | `field/field-scalar` | — | **keep** | |
| level `index` | VALUE u32 | assigned | `field/field-indexed` | — | **keep**: note that the text caps an index at 65535 while the wire carries u32; #1965 (one normative source) should state one width. | |
| `index_mode` SCALAR | `0` | assigned | the default | — | **keep** | |
| `index_mode` ELEMENT | `1` | assigned | `field/field-append`, `field/field-indexed` | — | **keep** | |
| `index_mode` WILDCARD (`[*]`) | `2` | assigned; "no operation performs" it | refused or `SCHEMA_NOT_FOUND` everywhere; `field/field-wildcard`, `fwd/fwd-wildcard-reject` | — | **delete** | F9 |
| field-chain depth | ≤ 8 | assigned | resolver | — | **keep** | |
| `subscribers` | field name | assigned | `fwd/fwd-write-subscriber-field` | — | **keep** | |
| `acl` | field name | assigned | `:acl` | — | **keep** | |
| `children` | field name | assigned | enumeration; `spec/create-child` | 4 (#1945) | **keep** | |
| `settings` | field name | assigned | `field/field-settings-app` | — | **keep** | |
| `schema` | field name | assigned | `conn/absent-endpoint-not-found` | — | **keep** | |
| `identity` | field name | assigned | `graph_fields.cpp` | — | **keep** | |
| `stats` | field name | assigned | `settings/stats-seam-*` | — | **keep** | |

## 10. `FWD` (05 §`0x0F`, RFC-0004 §B and §D)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| `op` | first child, VALUE u8 | assigned | `core/include/libtracer/op_resolve.hpp:fwd_op_t` | — | **keep** | |
| op READ | `0` | assigned | `fwd/fwd-read` | 4 (a read below a door reaches the remote) | **keep** | |
| op WRITE | `1` | assigned | `fwd/fwd-write-value` | 4 (the reply becomes a write) | **keep** | |
| op AWAIT | `2` | assigned | `fwd/fwd-await-timeout` | 4 (#1946) | **delete** | covered |
| op REPLY | `3` | assigned | `fwd/fwd-reply-result` | 4 (#1946) | **delete** | covered |
| `op` bits 7–6 | `0xC0` | reserved, MUST be zero | the `op & 0x3F` mask | — | **keep** | |
| `op` bit 7 mint request | `0x80` | retired by RFC-0029 §5.3 | `kFwdOpFlagMintRequest`; `fwd/fwd-mint-request` | RFC-0029 S2 | **delete** | covered |
| `dst` | PATH | required | every FWD | 1 (#1939) | **keep** | |
| `selector` | FIELD | optional | `fwd/fwd-write-subscriber-field` | — | **keep** | |
| `src` | PATH | required | every FWD; `fwd/fwd-src-accumulated` | 4 (carries the reply path) | **keep** | |
| empty `src` (unacknowledged) | zero-length PATH | assigned (RFC-0004 Am. 2) | the delivery leg | 4 (#1942 folds #1502) | **keep** | |
| `kind` RESULT / ERROR | VALUE u8 `0`/`1`, REPLY only | assigned | `reply_kind_t`; `fwd/fwd-reply-error` | 4 (#1946) | **delete** with REPLY. | covered |
| `await_timeout` | VALUE u64, AWAIT only | a hint (RFC-0004 Am. 3) | `op_resolve_walk.hpp` | 4 (#1946) | **delete** with AWAIT. | covered |
| trailing `PATH_REF_REVERSE` | `0x15` child | assigned | `fwd/fwd-reverse-mint` | RFC-0029 S5 | **keep** | |
| trailing reply `PATH_REF` | `0x14` child on a REPLY | retired by RFC-0029 §5.3 | `fwd/fwd-mint-reply` | RFC-0029 S2 | **delete** | covered |

## 11. `SPEC`, the creator endpoint, the connection vertex

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| SPEC `type` | NAME key + NAME value | assigned | `:children[]` creation; `spec/create-child` | 4 (#1945) | **keep** | |
| SPEC `name` | NAME key + NAME value | assigned | `conn/create-via-spec` | — | **keep** | |
| SPEC `config` | NAME key + SETTINGS | optional | `conn/create-via-spec` | — | **keep** | |
| creator endpoint `conn` | reserved name | assigned | `conn/remove-reserved-rejected` | — | **keep** | |
| removal write | `NAME{<name>}` | assigned | `conn/remove-via-name` | — | **keep** | |
| connection-vertex value | VALUE u8: DORMANT 0 … BIND_FAILED 5 | RFC-0014 §4 (outside the annexes) | `core/include/libtracer/transport_factory.hpp:link_state_t`; `conn/liveness-enum` | — | **keep**: #1965 should bring it into the normative text. | |

## 12. Route-handle frames (05 §route-handle, RFC-0004 §E.1)

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| `label` | VALUE u16, `0` = none | assigned, no vectors | `fwd_router.cpp` | 6 (#1950, #1951) | **delete** with COMPACT. | covered |

## 13. Discovery names

| Element | Codepoint | Status | Shipped use | Stage | Verdict | F |
| --- | --- | --- | --- | --- | --- | --- |
| mDNS service, v1 | `_libtracer._tcp` | committed by ADR-0002; named in 01 §versioning | none found: no reference module announces or browses | — | **rename** to `_tracer._tcp` with the protocol name (#1963 (name the protocol Tracer)); nothing ships it, so the rename is free now. | F12 |
| mDNS service, v2 | `_libtracer-v2._tcp` | committed by ADR-0002 | none found | — | **delete**: a future incompatible version picks its own name when it exists. | F12 |

Both F12 verdicts contradict ADR-0002, which keys the service name to the protocol integer.
F12 must amend ADR-0002 explicitly rather than override it in silence.

Nothing else from a public registry is on the wire: no port, URI scheme, media type or
WebSocket subprotocol was found. [#1967](https://github.com/avatarsd-llc/libtracer/issues/1967)
(IANA considerations) therefore needs one DNS-SD service-name registration and nothing else.

## Follow-ups

Proposed only. The maintainer files them after review and replaces each F-number here with the
issue number. Each delete names the vector that will show the element is refused or ignored.

| F | Title | Instrument | Verdicts | Vector that shows it |
| --- | --- | --- | --- | --- |
| F1 | SUBSCRIBER: drop the unread `capability` and `subscriber_id` children | amendment | 2 delete | `subscriber/unread-children-ignored` (new): a SUBSCRIBER with both children subscribes exactly as one without them. |
| F2 | SUBSCRIBER: a record without `target_path` is not an unsubscribe | erratum | 1 delete | `subscriber/no-target-refused` (new): written to `:subscribers[N]`, it answers `tr::schema::type_mismatch` and the slot stays. |
| F3 | `qos_settings`: drop `delivery_scope`, `batch_count`, `batch_window_ns` and the `reliability` bits | amendment | 4 delete | `subscriber/retired-qos-ignored` (new): the three keys and bits 0–1 set change nothing about delivery. |
| F4 | Per-vertex `delivery_mode` has no wire spelling | erratum | 1 delete | `settings/delivery-mode-not-found` (new): a `:settings.delivery_mode` write answers `tr::schema::not_found`. **Done** in PR [#2013](https://github.com/avatarsd-llc/libtracer/pull/2013). |
| F5 | POINT: drop the `description` child | amendment | 1 delete | `point/branch-write-description-refused` (new): a branch write carrying it answers `tr::schema::type_mismatch`. |
| F6 | One error-reply spelling: `STATUS{ERROR}` | erratum | 1 merge | `fwd/reply-error-bare-ignored` (new): a `kind=ERROR` reply with a bare ERROR payload is not surfaced as an error. |
| F7 | Error registry: retire `0x0041` and `0x0070`, rename `0x0042` to `tr::flow::gap` | amendment, folded into the stage-4 RFC (#1942) for `0x0041` | 2 delete, 1 rename | `errors/retired-code` (new): an ERROR carrying a retired code decodes and is reported as an unknown code. `errors/error-registered-detail` is re-spelled with a live code. |
| F8 | ACE: retire flags INHERIT_ONLY, NO_PROPAGATE, GROUP and rights DELETE, WRITE_OWNER | amendment | 5 delete | `acl/ace-retired-flag-refused` (new): an `:acl` write with flag `0x2` answers `tr::schema::type_mismatch`. `acl/ace-retired-right-grants-nothing` (new): an ACE granting only `0x10` or `0x80` admits no operation. |
| F9 | Index forms: drop `[*]` (`index_mode` 2) and the address-segment index production | amendment | 2 delete | `field/field-wildcard` becomes a refusal case: `index_mode` 2 answers `tr::path::invalid` in every context. |
| F10 | One wire form for a path: drop the VALUE-string spelling | erratum | 1 merge | `subscriber/target-as-value-refused` (new): a SUBSCRIBER whose target is a VALUE string is refused. |
| F11 | Retire type code `0x0D` ROUTER | amendment | 1 delete | `tlv-types/router-wrapped` is replaced by `tlv-types/retired-code-skipped` (new): a nested `0x0D` child is skipped, and an outer one answers `tr::schema::type_mismatch`. |
| F12 | Discovery: `_tracer._tcp`, and no pre-assigned v2 name | amendment (of ADR-0002 and the 01 §versioning sentence) | 1 rename, 1 delete | No frame vector applies: discovery is outside the vector corpus. The proof is the IANA registration request in #1967 (IANA considerations). |

Two vectors describe elements the spec has retired, and need new descriptions only. Their
bytes stay the same, so this is not a spec change:

- `tlv-types/settings-reliability` says "as the library encoder emits one" for a knob that
  RFC-0022 removed.
- `tlv-types/router-wrapped` names the deleted `router_wrap` helpers. F11 replaces it.
