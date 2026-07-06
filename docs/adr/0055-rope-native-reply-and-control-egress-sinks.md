# Rope-native reply and control egress: retire the `on_frame_rope` flatten

Status: **accepted** (2026-07-06 — maintainer-directed in the ADR-0053 ⑥ review). This
is the terminal slice of the [ADR-0053](0053-lazy-rope-backed-decode-view-partial-path-routing.md)
migration order ("⑥ — flatten sweep: remove remaining owning-path flatten call-sites;
span-tier flattens are legitimate and stay"), applied to the two egress sinks that still
force a whole-frame flatten: the originator **reply** callback and the **control-frame**
delivery path.

## Context

After ④b (routing + lazy resolver) and ⑤ (`compose → rope` emission + the rope-pinned
store), `fwd_router_t::on_frame_rope` routes a forward hop and resolves a request
terminus **directly off the rope**, zero-copy. One owning-path flatten remains — the
fallback for a frame that reaches this node as its destination and is **not** a request
the terminus resolves:

```cpp
// on_frame_rope, multi-link branch — the surviving interim flatten
const view_t flat = frame.flatten();
on_frame_impl(inbound_name, flat.bytes(), &flat);   // REPLY-at-originator + control
```

It exists because the two sinks `on_frame_impl` reaches for those frames both consume a
**contiguous, eagerly-decoded `tlv_t`**:

- **`reply_cb_`** — `std::function<void(const wire::tlv_t&)>`, set via the public
  `on_reply(...)`. A `FWD{REPLY}` whose accumulated return route is fully consumed is
  decoded and handed to the originator's callback.
- **`on_advertise` / `on_compact` / `on_nack`** — the route-handle control frames, each
  taking a `const wire::tlv_t&` (`on_compact` then re-encodes its payload child for
  `deliver_local` / a forwarding `encode_compact`).

Per [ADR-0052](0052-rope-aware-decode-sink-node-type.md) (ratified 2026-07-06), `tlv_t`
**remains** the eager, materialized, cross-core-parity representation, and `materialize()`
is the **sanctioned escape hatch** for a path that genuinely wants the eager tree.
`tlv_t.payload` is a borrowed contiguous `std::span`, so a `rope → tlv_t` decode is not a
cursor swap — it *requires* a contiguous backing buffer, i.e. the flatten. The question ⑥
answers is not "how do we decode a rope into a `tlv_t` without a flatten" (you cannot,
by ADR-0052's design) but **"who decides to materialize, and where."**

Today the *router* decides — it flattens every multi-link reply/control frame on the
consumer's behalf, before it knows whether the consumer even wants a tree. And every
in-tree `on_reply` consumer immediately calls `wire::encode(reply)` to get the **bytes
back** — a decode (router) + re-encode (consumer) round-trip to recover what the wire
already had. The flatten is misplaced work, not necessary work.

## Decision

**Move the materialize decision from the router to the consumer**, by making the reply
and control egress sinks rope-native. The router hands over the frame as its
scatter-gather `view::rope_t`; a consumer that wants the eager `tlv_t` invokes the
ADR-0052 escape hatch itself.

1. **`on_reply` becomes rope-native (breaking public API change).**
   `std::function<void(const wire::tlv_t&)>` → `std::function<void(const view::rope_t&)>`.
   The router performs **no decode and no flatten** for a reply. A consumer that only
   needs the bytes (every current in-tree consumer) takes `reply.flatten().bytes()` — a
   single-link reply is a zero-copy `only()`, a multi-link one pays exactly the one
   contiguous copy it was always going to pay, now **at the consumer, on demand**. A
   consumer that wants the tree calls `wire::decode(reply.materialize().bytes())`.

2. **Control-frame delivery is served off the rope.** `on_advertise` / `on_nack` read
   only a `u16` label — stitched via the existing `grammar::rope_cursor` loads, no
   flatten. `on_compact` still needs a **contiguous payload** for `deliver_local` and a
   forwarding `encode_compact`; that contiguity is an ADR-0052 §"legitimate flatten"
   (a transport-egress / local-store boundary), so it materializes **only the payload
   sub-rope**, not the whole frame, and only on the delivered-good-frame path.

3. **The `on_frame_rope` whole-frame flatten fallback is deleted.** Multi-link replies
   and control frames dispatch through their rope-native sinks; the span (`on_frame`)
   path is unchanged — a contiguous frame still decodes eagerly, and those **span-tier
   flattens are legitimate and stay** (ADR-0053 ⑥).

`tlv_t` is **not** touched — ADR-0052's cross-core parity and the ADR-0041 §2 span-arena
contract are preserved. This is purely a question of *where* the optional materialize
happens.

## Why breaking is acceptable here

`on_reply` is a reference-implementation host API (`fwd_router_t`), not a wire-format or
spec surface — no RFC, no conformance-vector change, no cross-core mirror. The blast
radius is six in-tree call sites (five transport/router tests + the `full_node` example),
each of which *shrinks*: `wire::encode(reply)` → `reply.flatten().bytes()` drops the
decode+re-encode round-trip. A public-API note lands in `core/CHANGELOG.md`. The
alternative — the router flattening on every consumer's behalf forever — is exactly the
"flatten as end state" ADR-0052 rejected.

## Considered alternatives

- **Keep the flatten; call it a legitimate escape hatch.** Tempting under ADR-0052's
  materialize-escape-hatch clause, but it puts the escape hatch in the *wrong actor*: the
  router materializes speculatively for a consumer that (today, always) did not want a
  tree. Rejected — the escape hatch belongs to whoever wants the eager form.
- **Hand the consumer a `tlv_view_t` (lazy tree) instead of a `rope_t`.** Rope-native and
  zero-copy, but `tlv_view_t` is forward-only and its ergonomics differ from `tlv_t`'s
  random-child access; every current consumer wants *bytes*, not a lazy tree, so a
  `rope_t` is the smaller, more honest surface. A future consumer that wants lazy tree
  access can adopt the rope as a `tlv_view_t` itself (`tlv_view_t::over(rope)`). Held as
  available, not imposed.
- **A second `on_reply` overload (keep both `tlv_t` and rope forms).** Does not delete
  the flatten (the `tlv_t` overload still forces it) and doubles the sink surface.
  Rejected.

## Consequences

- `on_reply`'s signature changes; `core/CHANGELOG.md` gets a breaking-change note; the
  six in-tree consumers migrate to `reply.flatten().bytes()` (a net simplification).
- `on_frame_rope` has no owning-path flatten left; the ADR-0053 ⑥ "flatten sweep" is
  complete for the net plane. The only remaining flattens are the **legitimate**
  span-tier decodes on the contiguous `on_frame` path and the on-demand payload
  materialize inside `on_compact` (a transport-egress boundary).
- `encode_compact` / `deliver_local` gain the ability to take a rope payload (or the
  materialized sub-rope) so the control path never flattens more than the payload it is
  about to hand to a link or store.
- The steady-state zero-copy requirement (ADR-0052 ratification point 1) now holds for
  the whole rope receive path, request **and** reply, with materialization occurring only
  at an explicit consumer/egress boundary.
