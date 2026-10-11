# Reference 24 — Modelling granularity: vertex, value, field

> **Status**: informative guide. This page states no rule of its own. It collects what the
> specification and the reference core already do about *granularity* (what a subscriber
> observes, what can be addressed, and what is notified), then gives modelling advice
> that follows from it. It is not a normative annex ([v1.md §3](../spec/v1.md#3-wire-format)
> lists those). Where this page and the specification disagree, the specification wins.
>
> **Audience**: anyone deciding how an application's data maps onto vertices, values and
> `:` fields.
>
> **See also**: [02-graph-model.md](02-graph-model.md) §subtree subscriptions and §schema and
> field discipline; [03-addressing.md](03-addressing.md); [05-protocol-tlvs.md](05-protocol-tlvs.md)
> §`0x04` and §`0x07`; [06-user-data-packing.md](06-user-data-packing.md);
> [22-backpressure-and-sizing.md](22-backpressure-and-sizing.md).

---

## 1. The vertex is the unit of observation

A subscription is an edge stored on a **vertex**, at `<vertex>:subscribers[]`. It observes that
vertex **and every descendant of it**. A leaf subscription is the trivial case, and there is no
wildcard grammar ([RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md);
[05 §`0x04`](05-protocol-tlvs.md) §subtree subscription).
Nothing narrows a subscription below a vertex. `:subscribers` takes exactly one selector
step, `[]` or `[N]`, and the record is stored and served whole
([04-communication-flows.md](04-communication-flows.md)). The field-index `[*]` can be
encoded, but no operation performs it ([03 §the index form on the wire](03-addressing.md#the-index-form-on-the-wire)).
Delivery selection is structural. The producer decides when to propagate, and a per-vertex
`delivery_mode` decides whether an *ancestor's* sweep includes the vertex
([RFC-0008](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0008-vertex-operations-assign-propagate.md)).
A subscriber chooses which vertex to subscribe to, and nothing finer.

### What a subscriber receives

The delivered payload is **the written TLV as-is**, at the granularity the producer chose. The
runtime does not re-encode it and adds no envelope. For a subscriber on vertex `V`:

| Event | What the subscriber of `V` receives |
| --- | --- |
| A plain write to `V` | `V`'s new value, whole. A write replaces the value (last writer wins). If the owner installed an admission filter, subscribers see the value it admitted (`core/src/graph.cpp:is the only value any subscriber can observe`). |
| A write to a descendant `W` | `W`'s written TLV. This is not `V`'s value and not a composed view of `V`'s subtree. A local callback can learn the concrete path out of band. A remote delivery carries no path tag; provenance travels in the data ([03 §subscriber identity](03-addressing.md#subscriber-identity-across-a-subtree)). |
| A branch write (a `POINT` tree) at `V` | The written `POINT` as-is, which covers only the nodes the writer included. Each landing vertex's own subscribers get the slice covering that vertex. Admission is all-or-nothing, but application is per leaf: there is **no branch transaction** ([05 §`0x07`](05-protocol-tlvs.md) §branch write). |
| *k* assigns, then one `propagate` (a stored-value vertex) | **One** delivery of the current stored value. The intermediate values are not delivered: coalescing is the default contract ([02 §assign, propagate](02-graph-model.md#assign-propagate-and-the-coalescing-sweep)). |
| `propagate` on a `STREAM` vertex | Every ring entry appended since the last flush, in order, each as its own delivery, each one whole written value (`core/src/graph.cpp:graph_t::deliver_current`). |
| `propagate(v, FOLD)` | One branch-write frame for the swept selection. Under the default `IF_NEWER` mode, only vertices assigned since the last sweep are in it, so an ancestor subscriber sees a partial tree. A covered leaf sees the same bytes it would see without folding. The fold refuses a selected value that is not a single trailer-less `VALUE`, and it refuses a selected `STREAM` ([02 §wire mapping](02-graph-model.md#wire-mapping)). |
| An application-composed batch | One value the application composed and swapped in, such as a `0x80` BATCH record. It is delivered whole like any value, and the graph never interprets it ([RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md) §4.1.3). |
| A subscribe that sets `durability_request` | One immediate delivery of the last-known value (the latch), then the ordinary stream ([05 §`0x04`](05-protocol-tlvs.md) §producer fan-out). |
| A write to any `:` field | Nothing (§2 below). |

**No delta, patch or partial-value delivery exists.** In particular:

- `delivery_scope` is a reserved key. No implementation reads it.
- There is no `ON_CHANGE` byte diff, and no rate or interval throttle.
- `batch_count` and `batch_window_ns` are retired. They are carried verbatim and read by nothing.
- The `delivery_class` bits are decoded but not honoured: whatever class a subscriber asks for,
  delivery follows the roles of the vertices on each side of the edge
  ([05 §`0x04`](05-protocol-tlvs.md), "Bits 6–7 are DECODED, not yet honoured").
- Per-element delivery is the subject of
  [RFC-0017](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0017-element-addressing-value-plane-index.md),
  which is a **draft** that nothing implements.
- Address-shift slicing ([03 §address-shift slicing](03-addressing.md#address-shift-slicing-replaces-wire-level-fragmentation))
  is an application pattern over separate child vertices. Its assembly settings are a design,
  and none is implemented.

## 2. `:` fields are read on demand and never observed

The field namespace is closed. It is `:subscribers`, `:acl`, `:children`, `:settings`, `:schema`,
`:identity` and `:stats`, and any other name answers `tr::schema::not_found`
(`core/src/graph_fields.cpp:graph_t::field_write`). No field has a subscription surface or an
await surface:

- **Subscribing to a field cannot be spelled.** A subscribe is a `SUBSCRIBER` written to a
  vertex's `:subscribers[]`. A `SUBSCRIBER` written to `:schema`, `:identity` or `:stats` answers
  `tr::schema::not_found`, because those fields have no write door. So does one written to a
  longer chain such as `:schema.subscribers[]`. Below `settings`, only a declared `app.<name>`
  field is writable.
- **Awaiting a field is refused.** A remote `FWD{AWAIT}` that carries any `:` selector answers
  `tr::schema::not_found` (`core/src/op_resolve_walk.hpp:A FIELD selector has no await surface`).
- **A field write is silent.** A field write, local or remote, does not wake `await`, does not
  advance the vertex's write sequence and does not propagate
  ([RFC-0010](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0010-owner-app-fields-and-schema.md) §C;
  `core/src/graph_fields.cpp:field write itself deliberately neither wakes`).

> **Caution: in the C++ host API, a field tail is ignored, not refused.** `graph_t::subscribe`
> and `graph_t::await` take a `path_t` and resolve only its vertex part
> (`core/src/graph.cpp:graph_t::subscribe(const path_t& src, subscriber_fn_t fn`,
> `core/src/graph.cpp:graph_t::await(const path_t& path`). A path such as `/x:schema` is
> accepted, and the call subscribes to, or awaits, the whole vertex `/x`. Never pass a field
> tail to these calls.

### Field values can change

Fields are read on demand. Whether a field's value can be cached depends on the field: none of
them pushes a change.

| Field | When it changes | Notes |
| --- | --- | --- |
| `:schema` | Whenever the owner replaces the vertex's field table. | Synthesized on every read: a protocol part (an empty `SETTINGS`, or a creator endpoint's declared catalog) plus an owner part projected from the installed field table ([05 §`0x07`](05-protocol-tlvs.md) §the `:schema` read). **No contract makes it immutable.** `graph_t::set_policy` replaces the table. Its documentation calls it a wiring-time call, but nothing enforces that, and a replacement delivers nothing. |
| `:identity` | Only when the application installs a different record. | Node-scoped, readable before authentication ([RFC-0011](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0011-node-identity-facet.md)). Re-installing it is configuration, but it is allowed. |
| `:stats` | On every read. | Live counters, sampled when read. Never writable, never awaitable. |
| `:subscribers[]` | On every subscribe, unsubscribe or eviction. | |
| `:children[]` | On every registration, creation or retirement. | An appearance shows up as the new child's first write, which bubbles to the parent's subscribers. **A disappearance is never notified** ([02 §vertex lifecycle](02-graph-model.md#vertex-lifecycle-registration-creation-retirement-revival), retirement notification). |
| `:acl` | On an `:acl` write. | Every later operation is checked against the new list. |
| `:settings.app.<name>` | On a field write, or whenever the owner's state changes, if the owner answers reads itself (RFC-0010 Amendment 4). | See §4. |

Cache a field only where the owner keeps it fixed. Re-read it after the owner announces a change
(§4).

## 3. Inside a value, nothing is addressable

A value is one TLV, read and written whole. A structured value (`opt.PL=1`) has child TLVs, and
the graph treats them as opaque bytes:

- **No path reaches a child TLV.** The address grammar reserves a segment index (`/x/v[3]`), but
  v1 gives it no carrier, and a bracketed segment is refused with `tr::path::invalid`
  ([03 §index forms](03-addressing.md#index-forms)). The field `[N]` indexes field-plane arrays
  such as `:subscribers[N]`, never a value. There is no `:value` field.
- **`:children[]` lists registered child vertices**, not the child TLVs of a value.
- **A read returns the stored TLV whole.** On a vertex with registered children, a plain read
  returns the composed branch read: one `POINT` tree over the child **vertices'** stored values
  ([RFC-0016](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0016-composed-branch-read.md)).
  It is a view, not a transaction, so values from different leaves can come from different moments.
- **A write replaces the whole value.** No member-wise write exists. A single write is atomic: a
  concurrent reader sees the whole old value or the whole new one
  ([03 §atomicity of multi-field writes](03-addressing.md#atomicity-of-multi-field-writes)).
- **Do not use `POINT` (`0x07`) as a record type.** A structured `POINT` written to a vertex is a
  *branch write* (`core/src/graph.cpp:is_branch_point`). It must be rooted at the target vertex,
  and its nodes may hold only a leading `NAME`, at most one `VALUE` and nested `POINT`s, so a record
  of `NAME`/`VALUE` pairs under `POINT` is refused with `tr::schema::type_mismatch`. Carry a record
  as a `VALUE` with `opt.PL=1`, or under a user-range type code. A vertex that should ride a
  `FOLD` sweep must store a single `VALUE`.

## 4. Field change notification

A write to a declared `:settings.app.<name>` field is reported **per field to the owner, and to
nobody else**:

- **Before the store**, the owner's admission seam `on_app_field_admit` can refuse or normalise the
  write.
- **After the store**, the owner's apply seam `on_app_field_write` fires with the field's key and
  the stored TLV. It fires for the owner's own writes and for remote writes alike
  (`core/src/graph_fields.cpp:if (auto seam = v->on_app_field_write(); seam)`).
- **Subscribers get nothing, and `await` does not wake.** Notification to consumers is at
  vertex granularity and is the owner's act. Once the change is applied, the owner announces it
  with an ordinary data write at the vertex. Subscribers receive one delivery and re-read the
  fields they care about. Consumers must not poll fields for change detection
  ([RFC-0010](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0010-owner-app-fields-and-schema.md) §C).
- **Writes to several fields are not atomic.** Each field write lands on its own. Knobs that must
  take effect together go in one `settings.app` leaf ([03 §atomicity of multi-field writes](03-addressing.md#atomicity-of-multi-field-writes)).

RFC-0010 Amendment 4 adds a read seam (`on_app_field_read`) that lets the owner answer reads
with its live value. It adds no notification.

## 5. Modelling guidance

These follow from §1–§4. They are advice, not requirements.

**Make a part its own vertex when:**

- it changes independently of its siblings, or often;
- it has its own consumers, who should not receive the rest;
- it needs different access. An ACL belongs to a vertex (its own ACEs plus inherited ones,
  [05 §`0x0A`](05-protocol-tlvs.md)). A field has no ACL slot, and it is gated by its vertex's
  rights;
- a consumer must `await` it, or it needs its own role (for example, a `STREAM` that keeps every
  sample) or its own `delivery_mode`.

**Make it a field of one structured value when:**

- its parts change together;
- readers must never see a half-applied update. Only a single write is atomic. A branch write
  across child vertices is not, and neither is a series of field writes.

**Make it a `:settings.app` field when** it is configuration or description that is read on
demand and has no observers of its own. A field rides its vertex without a subscriber list, an
ACL slot or a map entry, which makes it far cheaper than a vertex (RFC-0010 §D). If a field turns
out to need its own subscribers, promote it to a child vertex
([CONTEXT.md](../../CONTEXT.md) §field promotion). A consumer cannot add notification to another
device's field.

### Consequences

- **A large catalog held as one value resends everything on each change**, to every subscriber,
  however small the edit. Split it: fixed-size page vertices, or one child vertex per record. A
  subscriber on the parent still sees every record's writes by bubbling, and a join reads the
  current set with one composed read of the parent (which is itself the whole subtree) or with
  `:children[]`. Retiring a child is never notified. If records come and go, also write a small
  membership value on the parent when the set changes, so the parent's subscribers hear about it.
- **High-rate streams belong in their own vertices**, away from subtrees that slow or broad
  consumers subscribe to. Every write bubbles to every ancestor that has a subscriber, so a hot
  leaf under a widely watched parent pays one delivery per covering subscription
  ([03 §costs and obligations](03-addressing.md#costs-and-obligations)). A consumer that needs every
  sample makes its own target vertex a `STREAM`. A producer that wants amortization composes
  batches itself ([22-backpressure-and-sizing.md](22-backpressure-and-sizing.md), Recipe C).
- **A stored-value subscriber sees the latest value, not every value.** Do not encode meaning
  in transient intermediate states. Use a `STREAM` where every sample matters.
- **Announce configuration changes.** An owner that applies a field write, or replaces its field
  table, follows up with a data write at the vertex, so that consumers know to re-read.
