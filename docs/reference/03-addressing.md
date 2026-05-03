# Reference 03 — Addressing

> **Status**: draft, v0.1, 2026-05-03. Defines how vertices and fields are named, how subscriptions match paths, and how application-level slicing replaces wire-level fragmentation.
> **See also**: [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) for API rationale; [02-graph-model.md](02-graph-model.md) for the schema discipline that gives field names meaning.

---

## Path syntax

EBNF (using ABNF-like notation):

```
path        = root *segment-sep [ field-sep field-chain ]
root        = "/"
segment-sep = "/"
field-sep   = ":"
segment     = name [ index ]
name        = 1*64 ( UTF8-codepoint - reserved )
index       = "[" ( 1*5DIGIT / "*" / "" ) "]"
field-chain = field *( "." field )
field       = name [ index ]
reserved    = "/" / ":" / "." / "[" / "]" / "*" / "?"
DIGIT       = %x30-39
```

- All names are UTF-8, case-sensitive, **case-folded NOT performed** (Unicode normalization is the application's responsibility — `/Sensor/temp` and `/sensor/temp` are different paths).
- Maximum **single-name** length: 64 bytes (UTF-8 encoded).
- Maximum **total path** length: 1024 bytes.
- Maximum **segment depth**: 32 (matches the iterative-parser depth cap from [01-data-format.md](01-data-format.md)).
- Maximum **field-chain depth**: 8 (e.g., `:settings.transport_tcp.tls.cipher.suite` is at the limit).
- Maximum **index value**: 65535 (fits in u16).

A path that violates any limit MUST be rejected with `ERROR=INVALID_PATH`.

### Examples

```
/sensor/temp                           — a vertex
/sensor/temp:subscribers[0]            — a control field on a vertex
/sensor/temp:subscribers[]             — append-or-list view of subscribers
/sensor/temp:settings.reliability      — a nested control field
/sensor/temp:settings.transport_tcp.send_buf_kb  — module-namespaced field
/can-bridge/wheel-encoder/left         — a vertex behind a bridge
/camera/frame[7]                       — element 7 of an indexed-children vertex
/camera/frame[]                        — append target (publisher) or list (reader)
/i2c-bus/0x68/accel                    — peripheral on I²C bus 0x68
/                                      — the root vertex (rarely addressed directly)
```

### Index forms

- `[N]` (decimal integer 0..65535): a specific slot.
- `[]` (empty index): the array as a whole. Reads return a LIST; writes append to the next free slot.
- `[*]` (wildcard, **subscribe-only**): match any index. Valid only inside SUBSCRIBER PATHs.

### Reserved characters

The five characters `/ : . [ ]` plus the wildcards `*` and `?` cannot appear inside a NAME segment. Implementations MUST reject any NAME containing them with `ERROR=INVALID_PATH`.

(`?` is reserved for future single-character wildcard semantics; it is not in use in v0.1 but is reserved to keep the door open.)

---

## Field-path resolution

The `:` separator divides a path into the **vertex address** (left of `:`) and the **field chain** (right of `:`).

```
/sensor/temp:settings.deadline_ns
  └────┬────┘└─────────┬─────────┘
   vertex addr     field chain
```

Resolution proceeds in two stages:

1. **Resolve the vertex address** by walking the segment chain from the root. Each segment must match a child vertex name; index segments select indexed children.
2. **Resolve the field chain** against the vertex's schema (read `:schema` to enumerate). Each `.subfield` step descends one level; `[N]` selects a slot in an array-typed field.

If stage 1 fails: `ERROR=NOT_FOUND`. If stage 2 fails: `ERROR=SCHEMA_NOT_FOUND` for an unknown field name; `ERROR=NOT_FOUND` for an out-of-range index on an existing array field.

### Reading vs writing array slots

- `read("/x:subscribers[0]")` returns the SUBSCRIBER TLV at slot 0, or `STATUS=ERROR(NOT_FOUND)` if empty.
- `read("/x:subscribers[]")` returns a LIST of all populated SUBSCRIBER slots, in slot-order.
- `write("/x:subscribers[3]", tlv)` places the TLV at slot 3, replacing any existing entry.
- `write("/x:subscribers[]", tlv)` allocates the next free slot and places the TLV there. The caller can recover the chosen index by reading `:subscribers[]` and looking for their TLV (typically by including a unique subscriber-id NAME in the SUBSCRIBER record).

### Atomicity of multi-field writes

A single `write(path, tlv)` is atomic: a concurrent reader sees either the full prior state or the full new state at that path, not a partial mixture. To update multiple fields atomically, write a single LIST TLV containing all the fields to a parent path; the router applies the LIST as one operation.

```c
// Non-atomic (reader between calls sees inconsistent state):
tracer_write("/x:settings.reliability", tlv1);
tracer_write("/x:settings.deadline_ns", tlv2);

// Atomic (reader sees both fields update together):
tracer_write("/x:settings", list_tlv_containing(tlv1, tlv2));
```

---

## Wildcards (subscribe-only)

Wildcards are valid in two contexts:

- The PATH of a SUBSCRIBER TLV (the publisher's outgoing target).
- The PATH the subscriber is **listening for** when registering its subscription.

Wildcards are NOT valid in `tracer_read`, `tracer_write`, or `tracer_await` calls. Those resolve a single path.

### Match rules

- `*` matches **exactly one** path segment, of any value.
- `**` matches **zero or more** path segments. `**` MUST be the only wildcard at its position; `**a` and `a**` are invalid.
- `[*]` (inside an index) matches **any index** at that segment. Equivalent to using `*` for the whole segment if the parent has only indexed children.

### Examples

```
/sensor/*/temp                  matches /sensor/A/temp, /sensor/B/temp; not /sensor/A/B/temp
/sensor/**                      matches /sensor, /sensor/A, /sensor/A/B, /sensor/A/B/temp
/sensor/**/temp                 matches /sensor/temp, /sensor/A/temp, /sensor/A/B/temp
/camera/frame[*]                matches /camera/frame[0], /camera/frame[1], ...
/i2c-bus/*/accel                matches /i2c-bus/0x68/accel, /i2c-bus/0x6A/accel
```

### Match cost

A subscription with a wildcard requires the router to walk the wildcard table on every relevant `tracer_write`. Implementation guidance (not normative): pre-compute the matching set of wildcard subscriptions per concrete write path, cached, invalidated when subscriptions change. Acceptable cost for the typical fan-in being modest (≤ few dozen wildcard subscribers per topic).

### Subscriber identity in wildcard subscriptions

A wildcard subscriber receives a stream of TLVs from many concrete paths. The dispatcher SHOULD provide the matched concrete path as metadata when delivering — typically by encoding it in the SUBSCRIBER's target path or by attaching a NAME TLV in the delivery LIST. The exact mechanism is implementation-defined; the spec only requires that a subscriber can determine which concrete path produced each delivered TLV.

---

## Address-shift slicing (replaces wire-level fragmentation)

The wire format ([01-data-format.md](01-data-format.md)) deliberately omits fragmentation rules. The application-level mechanism is **address-shift slicing**: a logically large payload is split across **N child endpoints** with the **same timestamp**.

### Sender behavior

```
Logical message: 10 MB camera frame, timestamp T.

Publisher chooses slice size S = 64 KiB.
Number of slices N = ceil(10 MB / S) = 160.

For i in 0..159:
    write("/camera/frame[i]", VALUE{ts=T, bytes=slice_i})
```

Each slice is a complete, valid, independently-routable TLV. The publisher emits N writes; the router and transport see N separate dispatches.

### Receiver behavior

A subscriber registers once with a wildcard path:

```
write("/camera/frame[*]:subscribers[]", SUBSCRIBER{path=/local/handler, settings})
```

Each subsequent `write("/camera/frame[i]", ...)` matches the wildcard and produces a delivery to `/local/handler` with the slice index recoverable from the matched path.

The subscriber assembles the slices into a coherent group by **`(timestamp, index)`** pairing:

- All slices with the same `ts` belong to the same logical message.
- The slice's `index` (from the `[N]` in its address) gives its position within the logical message.
- A slice may arrive at any time within the deadline window.
- Loss of a slice is detected as a missing index in the timestamp group at deadline time.

### Subscriber assembly policies

The subscriber's QoS at `:settings.address_shift.*` controls assembly behavior. (Field names finalize at week 6 of [../plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md) once the cross-bus demo exercises them; documented here as the v0.1 design.)

| Field | Type | Default | Effect |
| ---- | ---- | ---- | ---- |
| `:settings.address_shift.assemble` | bool | false | If true, hold slices in a per-timestamp buffer until the group is complete or deadline expires; deliver one assembled message. If false, deliver each slice immediately as it arrives. |
| `:settings.address_shift.expected_count` | u32 | 0 (unknown) | If non-zero, declares N up-front; missing indices are detectable before deadline. |
| `:settings.address_shift.on_gap` | enum | `surface` | `surface` = deliver partial group with `STATUS=ADDRESS_SHIFT_GAP`; `drop` = silently discard incomplete groups; `wait_forever` = never give up (bounded by `queue_max_bytes`). |
| `:settings.deadline_ns` | u64 | unset | Per-group assembly deadline. After the deadline relative to the first observed slice, the group is finalized per `on_gap`. |

### Loss detection

Missing index `k` in a group with `expected_count = N` and observed indices `{0..N-1} \ {k}`: at deadline, the assembler emits `STATUS=ADDRESS_SHIFT_GAP` with `ERROR.detail = k`.

For groups without `expected_count`, the assembler treats the largest-observed-index + 1 as the implicit `N` at deadline (so a 100-slice group missing index 99 looks complete at slice 98). If this is unacceptable, the publisher MUST emit `expected_count` out-of-band or use the LIST-with-index-list pattern (declare the index set in a leading `/camera/frame:manifest` write).

### Why this is good

- **Lossless transport composition.** Whatever the transport does (drop a UDP datagram, lose a CAN frame), each slice is independently lost or delivered. No reassembly state to corrupt.
- **No special FRAGMENT type code.** The wire format from [01-data-format.md](01-data-format.md) doesn't need a fragment-with-reassembly-metadata type; the addressing scheme carries it.
- **Stream processing is natural.** The subscriber decides whether to assemble or to process as a stream; the publisher doesn't impose either choice.
- **Per-slice priority and QoS.** The addressing scheme lets a publisher tag different slices with different priorities (e.g., camera I-frames at high priority, P-frames at low) by writing them to differently-configured `ep[N]` slots.

### Why this is hard

- **Index allocation discipline.** The publisher must agree with subscribers on what `[N]` means (byte offset / slice_size? row index? sample index in a window?). This is an application-layer convention; libtracer does not impose semantics.
- **Wildcard matching cost** (already discussed above).

---

## Address scopes: local, bridged, global

The same path can resolve differently depending on which transport produced the write. The protocol distinguishes three scopes:

### Local scope

A path resolves within the host's own graph. No bridge prefix. Applies to:

- In-process publishers and subscribers on the same node.
- Vertex paths created by application code on this node.
- Module-exported vertex paths (e.g., `transport_i2c` exposing `/i2c-bus/0x68/accel`).

### Bridged scope

A path is **prefixed** with the bridge's mount point when the bridge republishes incoming TLVs. A bridge configured with `mount = "/can-bridge"` and `source = "transport_can"` republishes a CAN-borne write to `/sensor/wheel/left` as `/can-bridge/sensor/wheel/left` on the local graph.

Subscribers on the local graph see the prefixed path. The original path on the source side is not visible past the bridge unless the bridge is configured to publish a manifest.

### Global scope

The "global" scope is the union of all hosts' local + bridged graphs. There is no single authority that owns it; it is a logical view assembled by traversing peer-id mounts.

A common convention (not normative): each peer's data lives under `/peer/{peer_id}/...` on every other host. The bridge configuration `mount = "/peer/{peer_id}"` interpolates the connecting peer's announced node name and creates one mount per remote peer. This keeps the global graph navigable without name collisions.

### Collision rules

When two transports / bridges would mount data at the same local path:

- **First-binder wins**: the first transport to bind a vertex name owns it. Subsequent attempts return `ERROR=PATH_IN_USE` (a yet-to-be-assigned error code in the `0x0C..0x7F` reserved range).
- Configuration MAY use `mount` prefixes to avoid collisions explicitly (`/can-bridge`, `/tcp-bridge`).
- For peer-id mounts (`/peer/{peer_id}`), uniqueness comes from the peer-id namespace. Conflicting peer-ids on the network are a discovery-layer problem, not an addressing problem.

---

## Path canonicalization

Two textually-different paths that name the same vertex MUST canonicalize to the same internal representation:

- Trailing slashes: `/sensor/temp/` and `/sensor/temp` are the same. Implementations SHOULD strip trailing slashes during parse.
- Empty segments: `/sensor//temp` is **invalid**, not equivalent to `/sensor/temp`. Reject with `ERROR=INVALID_PATH`.
- The root path is exactly `/`. `//` and beyond are invalid.

Field paths do not have a trailing-separator equivalent; `:settings.` (trailing dot) is invalid.

UTF-8 normalization: implementations MAY normalize path bytes to NFC at the parse boundary, but MUST be consistent: normalized paths and pre-normalized paths from peers must round-trip without collision. The recommended choice is to NOT normalize and to require senders to canonicalize before transmission. (Application authors generally use ASCII-only path components, so this is rarely an issue in practice.)
