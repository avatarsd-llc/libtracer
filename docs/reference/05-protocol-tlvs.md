# Reference 05 — Protocol-Defined TLVs

> **Status**: draft, v0.1, 2026-05-03 (revised same day). Per-TLV byte-precise specification for every type code in the core-reserved range. The header layout, options bits, fixed-width length, and trailer (TS + CRC) are in [01-data-format.md](01-data-format.md); this document specifies what each type code's payload looks like.

---

## Type code allocation summary

| Range | Use |
| ---- | ---- |
| `0x00` | Reserved sentinel; never a valid TLV |
| `0x01` – `0x1F` | Core protocol types (this document) |
| `0x20` – `0x7F` | Reserved for future core extensions |
| `0x80` – `0xFF` | User-defined application payload types |

Currently assigned: `0x01` – `0x0D` (13 types). The remaining `0x0E` – `0x1F` are reserved for v0.1 fast-track additions; `0x20` – `0x7F` is the long-term registry.

The names below match the C enum in [`libtracer/tlv.h`](../../libtracer/tlv.h).

---

## `0x01` — VALUE

Opaque application payload. No protocol-imposed structure; the bytes are whatever the publisher and subscriber agreed on out-of-band.

### Payload layout

```
[ payload bytes — application-defined ]
```

The payload is a contiguous, untouched user region. Wire-time TS and CRC live in the optional trailer per [01-data-format.md](01-data-format.md), not interleaved with the payload.

### Defaults

- `opt.PL = 0` (payload is opaque, not nested).
- `opt.CR` recommended `1` for any non-loopback transport.
- `opt.TS` recommended `1` when wire-time-stamping matters (latency telemetry, dedup tie-breaking).
- For application-domain timestamps, embed a sibling `TIME` TLV inside a wrapping `LIST` instead.

### Where it appears

- Body of normal `tracer_write` / `tracer_read`.
- Inside `SUBSCRIBER` records as the configuration scalar.
- Inside `SETTINGS` LISTs as field values.
- Inside `STATUS` LISTs as error-detail bytes.

### Validation

- No application-level validation by the core.
- The receiver MUST validate `length` against the available buffer before reading payload bytes.

### Hex example

5-byte payload `AA BB CC DD EE`, CRC enabled, no wire-time:

```
01 10 05 00 00 00 AA BB CC DD EE [crc:4]
^  ^  ^^^^^^^^^^^ ^^^^^^^^^^^^^^  ^^^^^
|  |  length = 5  payload          trailer_crc (CRC-32C over payload)
|  opt = 0x10 (CR=1)
type = 0x01 VALUE
```

`6 (header) + 5 (payload) + 4 (trailer_crc) = 15 bytes`.

Same payload with wire-time-stamp:

```
01 30 05 00 00 00 AA BB CC DD EE [ts:8] [crc:4]
^  ^  ^^^^^^^^^^^ ^^^^^^^^^^^^^^  ^^^^^  ^^^^^
|  |  length = 5  payload          ts     CRC over payload+ts
|  opt = 0x30 (TS=1, CR=1)
type = 0x01 VALUE
```

`6 + 5 + 8 + 4 = 23 bytes`.

---

## `0x02` — NAME

A single name segment. UTF-8 bytes, **no NUL terminator on the wire**.

### Payload layout

```
[ N bytes UTF-8 ]
```

### Constraints

- Length: 1..64 bytes (per [03-addressing.md](03-addressing.md) §path syntax).
- MUST NOT contain reserved characters (`/ : . [ ] * ?`).
- MUST be valid UTF-8. Invalid byte sequences MUST be rejected with `ERROR=INVALID_PATH`.

### Where it appears

- Inside PATH TLVs (one NAME per segment).
- Inside SETTINGS LISTs as field-name keys.
- Inside SCHEMA LISTs as field labels.
- Wherever a "label" is needed inside a structured TLV.

### Hex example

NAME "sensor" (6 bytes), no trailer (typical when nested inside a LIST whose outer trailer covers everything):

```
02 00 06 00 00 00 73 65 6E 73 6F 72
^  ^  ^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^
|  |  length = 6  "sensor"
|  opt = 0 (no PL, no TS, no CR)
type = 0x02 NAME
```

`6 (header) + 6 (payload) = 12 bytes`.

---

## `0x03` — DESCRIPTION

Free-form UTF-8 human-readable description of a vertex or field. Optional in every context; tooling shows it to operators.

### Payload layout

```
[ N bytes UTF-8 ]
```

### Constraints

- Length: 0..1024 bytes recommended; no hard upper limit beyond `length` field range.
- MUST be valid UTF-8.

### Where it appears

- `<vertex>:description` field.
- Inside SCHEMA LISTs annotating fields.
- Inside ERROR TLVs as the human-readable detail.

---

## `0x04` — SUBSCRIBER

Subscription record. The presence of a SUBSCRIBER TLV at `<vertex>:subscribers[N]` causes the router to fan out future writes to that vertex to the subscriber's target path.

### Payload layout

Always a LIST (`opt.PL=1`). The LIST contains, in order:

```
LIST {
  PATH        target_path     ; required — where to dispatch matched writes
  SETTINGS    qos_settings    ; optional — QoS overrides for this subscription
  ACL         capability      ; optional — capability token if enforced
  NAME        subscriber_id   ; optional — opaque ID for self-identification
}
```

### Where it appears

- `<vertex>:subscribers[N]` slot, one per subscription.
- Inside `<vertex>:subscribers[]` reads (returned as a LIST of SUBSCRIBER TLVs).

### Validation

- `target_path` MUST be a syntactically valid path (per [03-addressing.md](03-addressing.md)).
- A SUBSCRIBER with no `target_path` is treated as "clear this slot" (unsubscribe sentinel).

### Future extensions

The optional fields after `target_path` may grow. New optional sub-fields MUST appear after the existing ones in the LIST and MUST be NAME-tagged so older parsers can skip them.

---

## `0x05` — LIST

Ordered container of nested TLVs. The graph-node primitive of the same-substrate insight.

### Payload layout

```
[ child TLV 1 ] [ child TLV 2 ] [ ... ] [ child TLV K ]
```

Each child is a complete TLV (header + length + payload), concatenated end-to-end. Children may have any type code, including nested LIST.

### Header settings

- `opt.PL` MUST be `1`. (A type-LIST TLV with `PL=0` is invalid.)
- The `length` field gives the total byte size of all concatenated children.

### Constraints

- Nesting depth ≤ 32 (per [01-data-format.md](01-data-format.md) §iterative parsing).
- Number of children: unbounded by the protocol; bounded by `length` and per-application limits.

### Where it appears

- Wherever structured data is needed: SUBSCRIBER, SETTINGS, ACL, STATUS, schema, vertex enumeration.

### Hex example

A 2-element PATH (which is a LIST containing two NAME TLVs) for `/sensor/temp`, with outer CRC:

```
06 50 16 00 00 00 [inner 22 bytes] [crc:4]
^  ^  ^^^^^^^^^^^
|  |  length = 22 (sum of two child TLVs)
|  opt = 0x50 (PL=1, CR=1)
type = 0x06 PATH

  Inner LIST contents (22 bytes):
  02 00 06 00 00 00 73 65 6E 73 6F 72   ← NAME "sensor", 12 bytes
  02 00 04 00 00 00 74 65 6D 70           ← NAME "temp",  10 bytes
```

`6 (outer header) + 22 (inner LIST) + 4 (outer CRC) = 32 bytes total`. The inner NAMEs carry no trailer of their own; the outer CRC covers their bytes.

(PATH is a specialization of LIST; see `0x06` PATH.)

---

## `0x06` — PATH

A hierarchical address as a LIST of NAME TLVs. Distinct from raw LIST so the parser can validate path-segment constraints up-front.

### Payload layout

```
LIST {
  NAME segment_1
  NAME segment_2
  ...
  NAME segment_K
}
```

### Header settings

- `opt.PL` MUST be `1`.

### Constraints

- Each child MUST be a NAME TLV (`type=0x02`); other types in the LIST are invalid in PATH context.
- Total path length (sum of NAME bytes + segment separators) ≤ 1024 bytes.
- Segment count ≤ 32.

### Where it appears

- Inside SUBSCRIBER as `target_path`.
- As the PATH form of `tracer_read`/`write`/`await` arguments when the path is constructed programmatically (the C API also accepts string form for ergonomics).
- Inside ROUTER for bridged-source path metadata.

### Note on string form vs PATH-TLV form

A path may be expressed two ways:

- **String form**: `"/sensor/temp"` — a UTF-8 byte string with `/` separators. Used at the API surface for ergonomics. Stored as a single VALUE TLV when transported as data.
- **PATH-TLV form**: a PATH TLV (LIST of NAMEs). Used inside structured TLVs (SUBSCRIBER, ROUTER) where the parser needs to validate segments individually.

Both forms canonicalize to the same internal representation. Implementations MUST accept either form where a path is expected.

---

## `0x07` — POINT

Endpoint definition: a vertex's full descriptor as a structured TLV. Used for vertex enumeration and replication snapshots.

### Payload layout

```
LIST {
  NAME           vertex_name        ; required — the leaf segment
  DESCRIPTION    description        ; optional
  SETTINGS       default_settings   ; optional
  LIST           subscribers        ; optional — current subscribers as LIST of SUBSCRIBER
  LIST           children           ; optional — child POINTs (recursive)
}
```

### Where it appears

- Returned by `read("/some/parent")` to enumerate children.
- Used by the future recorder/replay module to snapshot vertex state.
- Used by discovery modules announcing exported vertex trees.

### Constraints

- `opt.PL` MUST be `1`.
- The `vertex_name` MUST be the first child.
- Nested `children` LIST entries MUST themselves be POINT TLVs.

---

## `0x08` — ERROR

A single error condition. Used inside STATUS TLVs (which may carry zero or more ERRORs) and as the response payload for failed `read`/`write`/`await` calls.

### Payload layout

```
[ u8 error_code ]
[ optional DESCRIPTION (UTF-8) ]
[ optional VALUE (binary detail, error-code-specific) ]
```

The error code is always the first byte. Optional follow-on TLVs (DESCRIPTION, VALUE) are nested by setting `opt.PL=1` and packing them into the LIST after the leading code byte. (For implementers: the leading u8 is treated as a single-byte payload prefix; the rest of the payload is the LIST. This packing is deliberately compact for the common case of "code only.")

### Error code registry

```
0x00  OK                   Operation succeeded (rarely sent — empty STATUS implies OK)
0x01  NOT_FOUND            Path does not resolve to a vertex
0x02  PERMISSION_DENIED    ACL rejected the operation
0x03  INVALID_PATH         Malformed PATH or non-UTF-8 NAME
0x04  TYPE_MISMATCH        Payload type incompatible with endpoint schema
0x05  CRC_FAIL             Wire CRC did not match
0x06  VERSION_MISMATCH     opt.VR set higher than receiver supports
0x07  BACKPRESSURE         Subscriber queue full; sample dropped per QoS
0x08  TIMEOUT              No response within deadline
0x09  TRANSPORT_DOWN       Underlying transport disconnected
0x0A  SCHEMA_NOT_FOUND     Field read on a vertex that does not expose it
0x0B  ADDRESS_SHIFT_GAP    Missing index in an address-shift group at deadline
0x0C  TRUNCATED            TLV stream ended mid-frame
0x0D  NESTING_TOO_DEEP     LIST nesting exceeded depth cap
0x0E  PATH_IN_USE          Bind attempted on an already-owned vertex name
0x0F  – 0x7F  reserved for future core
0x80  – 0xFF  user-defined
```

### Where it appears

- Inside STATUS TLVs (zero or more ERRORs per STATUS).
- As inline reply payload in implementations that opt to skip the STATUS wrapper.

---

## `0x09` — STATUS

Communication status / response signal. An empty STATUS means OK; a non-empty STATUS contains one or more ERROR TLVs and optional DESCRIPTION text.

### Payload layout

When empty: `length = 0`. (Smallest valid STATUS is the 7-byte empty-OK form.)

When non-empty:

```
LIST {
  ERROR        first_error
  ERROR        second_error      ; optional
  DESCRIPTION  human_message     ; optional
  ...
}
```

### Header settings

- Empty STATUS: `opt.PL = 0`, `length = 0`.
- Non-empty STATUS: `opt.PL = 1`.

### Where it appears

- Synchronous return from `read` / `write` / `await` on failure.
- Asynchronous signal at `<vertex>:status` when subscribers should be notified of liveness/deadline/transport events.
- Sentinel TLV used to clear subscriber slots (write empty STATUS to `:subscribers[N]`).

### Hex example

Empty STATUS=OK (the smallest valid libtracer TLV — used as the unsubscribe sentinel and the implicit OK reply):

```
09 00 00 00 00 00
^  ^  ^^^^^^^^^^^
|  |  length = 0
|  opt = 0  (no PL, no TS, no CR)
type = 0x09 STATUS
```

**6 bytes total.** No trailer.

---

## `0x0A` — ACL

Access control list — a collection of capabilities granting permissions on a vertex. Stored at `<vertex>:acl`.

### Payload layout

```
LIST {
  LIST {
    NAME      subject       ; the holder of this capability
    VALUE     permissions   ; u8 bitfield: READ=0x1, WRITE=0x2, SUBSCRIBE=0x4
    VALUE     expires_ns    ; optional u64 expiration timestamp
  }
  LIST {
    ...                     ; next capability
  }
}
```

### Header settings

- `opt.PL = 1`.

### Where it appears

- `<vertex>:acl` field.
- ACL enforcement is performed by the `security_acl` module (post-MVP per [../plans/06-modules-executor-security-gui.md](../plans/06-modules-executor-security-gui.md)). The TLV layout is **structurally defined** in v0.1 even though enforcement is deferred.

### Constraints

- A vertex without an `:acl` field defaults to "no restrictions" (when `security_acl` is not loaded) or "deny by default" (when `security_acl` is loaded with strict mode).

---

## `0x0B` — SETTINGS

QoS and configuration block. A LIST of NAME-keyed values describing the writable fields under `:settings`.

### Payload layout

```
LIST {
  NAME "reliability"       VALUE u8
  NAME "durability"        VALUE u8
  NAME "history_keep_last" VALUE u32
  NAME "deadline_ns"       VALUE u64
  NAME "priority"          VALUE u8
  NAME "queue_max_bytes"   VALUE u32
  ; module-namespaced fields:
  NAME "transport_tcp"     LIST { NAME "send_buf_kb" VALUE u32 ... }
  ...
}
```

### Header settings

- `opt.PL = 1`.

### Where it appears

- `<vertex>:settings` for atomic multi-field reads/writes.
- Inside SUBSCRIBER as the `qos_settings` sub-field for per-subscription overrides.

### Validation

- Unknown NAMEs in the LIST MUST be either (a) ignored if module-namespaced and the module is not loaded, or (b) rejected with `ERROR=SCHEMA_NOT_FOUND` if in the core namespace.
- Type mismatches (e.g., a u32 where u8 expected) MUST return `ERROR=TYPE_MISMATCH`.

### The five core QoS knobs

(Full semantics in [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) §QoS knobs.)

| Field | Type | Default | Effect |
| ---- | ---- | ---- | ---- |
| `reliability` | u8 | 0 (best-effort) | 1 = reliable, transport-dependent guarantee |
| `durability` | u8 | 0 (volatile) | 1 = transient-local, late joiners see history |
| `history_keep_last` | u32 | 1 | Samples retained for transient-local |
| `deadline_ns` | u64 | unset | Maximum interval between writes; missed = STATUS=TIMEOUT |
| `priority` | u8 | 128 | Transport hint; 0 = lowest, 255 = highest |

---

## `0x0C` — TIME

64-bit absolute timestamp, nanoseconds since Unix epoch (1970-01-01 00:00:00 UTC).

### Payload layout

```
[ u64 timestamp_ns_le ]   ; 8 bytes, little-endian
```

### Where it appears

- Inside LIST TLVs as a sibling of VALUE when application-domain timestamps matter (sample-acquisition time, sensor exposure window, control deadline). Multiple TIME TLVs in one LIST is permitted; semantics are application-defined (typically discriminated by a sibling NAME).
- The wire-trailer `opt.TS=1` (see [01-data-format.md](01-data-format.md)) is **transport-time** — it tells you when the sender put the TLV on the wire. That is a different concern from application-domain time and the two SHOULD NOT be conflated.

### Constraints

- u64 wraparound: year 2554 (584 years from 1970). Acceptable.
- Negative (pre-epoch) values: not representable; reject with `ERROR=INVALID_PATH` (no dedicated INVALID_TIME code).

### Hex example

A LIST containing a TIME and a VALUE, with outer CRC:

```
05 50 1A 00 00 00 [inner 26 bytes] [crc:4]
^  ^  ^^^^^^^^^^^
|  |  length = 26
|  opt = 0x50 (PL=1, CR=1)
type = 0x05 LIST

  Inner LIST contents (26 bytes):
  0C 00 08 00 00 00 00 00 00 00 00 00 00 00 00   ← TIME, 14 bytes
  ^  ^  ^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^
  |  |  length = 8  u64 = 0 (epoch)
  |  opt = 0
  type = 0x0C TIME

  01 00 04 00 00 00 DE AD BE EF                  ← VALUE, 10 bytes
```

`6 (outer) + 26 (inner) + 4 (CRC) = 36 bytes`.

---

## `0x0D` — ROUTER

Bridge / router metadata. Attached to TLVs that traverse a bridge so the recipient can detect cycles, route preferentially, and surface diagnostics.

### Payload layout

```
LIST {
  NAME      origin_peer_id    ; UUIDv4 or device-derived ID of the original sender
  TIME      origin_timestamp  ; required for cycle dedup
  VALUE     hop_count         ; u8, incremented at each bridge
  NAME      transport_label   ; optional, e.g., "transport_can"
  VALUE     route_cost        ; optional u16, application-defined metric
  PATH      original_path     ; optional, the path on the source side before mount-prefix
}
```

### Header settings

- `opt.PL = 1`.

### Where it appears

- Attached to bridged TLVs (inside the LIST that wraps the original payload, OR carried as a sibling at the top-level when the bridge wraps the original TLV).

### Cycle handling

The `(origin_peer_id, origin_timestamp)` pair is the dedup key. A receiving bridge maintains a recent-set of seen pairs; TLVs already seen are dropped silently. Recommended recent-set size: `deepest_expected_route_fanout × longest_expected_delivery_window` (per [07-host-embedding.md](07-host-embedding.md) §cycle handling).

### Constraints

- `hop_count` SHOULD start at 0 at the source bridge and be incremented by each subsequent bridge. A bridge encountering `hop_count >= MAX_HOPS` (recommended 32) MUST drop the TLV and emit a local `STATUS=ERROR(NESTING_TOO_DEEP)`.

---

## Reserved range (`0x0E` – `0x1F`)

Currently unassigned. Allocated on a fast-track basis during v0.1 if a clear need emerges. Candidate uses:

- `MANIFEST` — explicit declaration of an `expected_count` for an address-shift group.
- `CAPABILITY` — opaque capability token (lighter than full ACL).
- `HEARTBEAT` — explicit liveness ping (currently subsumed by writes to `:liveness.last_seen_ns`).

Receivers MUST handle unknown codes in this range per the forward-compatibility rules of [01-data-format.md](01-data-format.md) §forward / backward compatibility.

---

## Reserved range (`0x20` – `0x7F`)

Long-term registry for future core extensions, post-v0.1. Allocation procedure: PR against this document with rationale + byte spec; implementer review; assignment.

---

## User range (`0x80` – `0xFF`)

128 type codes the protocol does not opine on. Senders and receivers agree out-of-band. Recommended convention: register a project-specific "magic" prefix (e.g., 4-byte UUID-derived bytes at the start of the payload) so multiple unrelated user types can coexist on the same wire without collision.
