# External subscription mutations are observable, at the admission door

Status: **ACCEPTED (2026-08-06).** Adds `graph_t::set_subscription_observer` and `sub_event_t` to the L4 host API. No wire change and no new wire operation — the events describe `:subscribers[]` writes the protocol already carries, and the observer is a *local, owner-facing* API in the [RFC-0009](../spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §A.1 sense. Rests on [ADR-0049](0049-field-write-single-subscriber-admission-door.md)'s single admission door and [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md)'s caller context; a `core/CHANGELOG.md` note covers the API addition.

## Context

An owner can ask "who is subscribed to this vertex, right now?" — `read_subscribers`, per vertex, on demand. It cannot ask "tell me when that changes." Two downstream needs are blocked by exactly that gap:

- **Demand-driven production.** [ADR-0049](0049-field-write-single-subscriber-admission-door.md) and `graph_t::has_subscribers` give a producer a cheap *level* to gate delivery on, but nothing edge-triggered: a producer that wants to *start a source* (spin up a sampler, open a bus, raise a duty cycle) when the first remote consumer arrives has to poll `has_subscribers` on a timer, which is both a wakeup the MCU did not need and a latency floor on the thing it is trying to be responsive about.
- **Fan-out inventory.** An app projecting "what is this node feeding, and to whom" — a `/system/links`-style vertex, an operator UI — must today walk every vertex and re-read every slot on a timer to notice one peer's subscribe.

The pressure is one-directional. The events an owner needs are the ones a *peer* caused; the ones the owner's own wiring code caused it already knows about, because it made the call. Reporting both would make the observer fire during the owner's own `subscribe()` at setup, which is noise at best and a re-entrancy hazard at worst (an observer that reacts by wiring more edges).

## Decision

**One app-installable, synchronous observer, fired from the ADR-0049 admission door and the `:subscribers[N]` clear, for EXTERNAL mutations only — where "external" is exactly a non-empty ADR-0018 caller context.**

- **`sub_event_t`** carries `{kind, producer, target, link, slot}`. `kind` is `ADDED` / `REMOVED`. `producer` and `target` are **canonical keys** (`wire::key_view_t` over concatenated NAME records), not slash-spelled strings: that is the form the graph addresses by, and rendering is the consumer's choice rather than a cost the event imposes. `link` is this node's NAME for the transport the op arrived on. All three are BORROWED for the call.
- **`target` is decoded from the record**, not from the parsed slot. `subscribe_wire` deliberately drops `target_key` for a remote binding (delivery rides the return route, [RFC-0004](../spec/rfcs/0004-remote-operation-addressing.md) §D), but the `PATH` the peer sent is still what an observer wants — it is the consumer's own spelling of itself. Absent or malformed `PATH` ⇒ an EMPTY target and the event still fires; the mutation happened either way.
- **One firing site for every ADD.** `admit_subscriber` is the single door every subscribe lands in (ADR-0049), so the wire `:subscribers[]` APPEND that binds a remote subscriber, the one that names a local target, and a `[N]` replace all report through one call — the event stream cannot diverge per entry point for the same reason the SUBSCRIBE gate and the durability latch cannot.
- **The `[N]` clear fires `REMOVED`**, and a `[N]` replace of a LIVE slot fires `REMOVED` then `ADDED` in that order: [RFC-0009](../spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §D.1 makes a replace two facts, and reporting only the add would leave an observer's inventory holding an edge that no longer exists.
- **Synchronous, on the resolver's thread, outside every graph lock**, after the mutation has landed and after the durability latch has been dispatched. The observer must be cheap and must not re-enter the graph. Deferral is the app's job, as it is for `set_remote_delivery_sink`.
- **Set once at wiring time**, read-only afterwards — the `set_remote_delivery_sink` / `set_subject_resolver` contract. Null by default: one null check on the subscribe path, nothing anywhere else.

### Why the caller context is the right discriminator

Because it is not a new one. ADR-0018 already defines the caller context as "this node's NAME for the inbound link a remote FWD arrived on, or empty for a local API call," and the SUBSCRIBE gate already runs under it — so the set of ops an observer sees is *by construction* the set the ACL treats as remote. Inventing a second `is_external` flag would have created a way for the two to disagree, and the disagreement would be silent.

It also lands the discrimination in **one function**. `observing_subscriptions(caller)` is the whole of it; every firing site asks it, and no site can accidentally fire for a local door.

### Two silences, on purpose

- **`evict_link_edges` emits nothing.** The transport plane's link-teardown hook (RFC-0009 §D) drops *k* edges of one departed link in a batch. It is a local host API, not an op, and it has no caller context — so it does not meet the external test, and synthesizing one would mean minting per-edge events for a fact the app learns more directly. **An app maintaining a live subscription inventory must therefore treat its own link-down signal as the removal for every edge of that link.** This is a real obligation, documented on the declaration, not an oversight.
- **`unsubscribe(subscription_t)` emits nothing**, for the same reason both `subscribe()` sugars do not: it is a local door, and its caller holds the handle it is removing.

## Alternatives rejected

- **A per-vertex observer, or a subscribe-to-`:subscribers` meta-edge.** Both push the "which vertices do I watch" question onto the app before it can answer "what exists" — and a meta-edge would be a wire-visible construct, which needs an RFC and buys nothing the local API does not already give the owner.
- **An event QUEUE the app drains.** Then the graph owns an unbounded buffer whose size a peer chooses, on a target where that is exactly the thing #477/#551 spent two ADRs removing. A synchronous callback makes the buffering decision — and its bound — the app's, where the memory budget lives.
- **Reporting local mutations too, with an `is_external` field.** Rejected on the re-entrancy hazard: the owner's setup-time `subscribe()` calls would fire an observer that is very likely to wire more edges, and the first thing every app would write is `if (!e.is_external) return;`.
- **Firing from `field_write` and `subscribe_wire` separately** rather than from `admit_subscriber`. That is two sites for one fact, which is the divergence ADR-0049 exists to prevent, and it would have missed the `[N]` replace's ADD.

## Consequences

- An app can drive production from demand with no polling, and can project the fan-out graph as live state.
- The observer is on the resolver's critical path for a `:subscribers[]` write: a slow observer slows every peer's subscribe. Control-plane frequency, and the warning is on the declaration, but it is a real coupling that a queue-based design would not have had.
- Subscriber removals caused by **link teardown** are not events. Any inventory built on this observer is correct only when joined with the app's own link-down signal.
- A pre-existing asymmetry becomes visible through this API and is NOT changed here: a wire `:subscribers[]` **APPEND** stores the edge's link NAME (`subscribe_wire`), while a wire `:subscribers[N]` **REPLACE** stores only the gate context — so a replaced edge carries no link and `evict_link_edges` never matches it. The observer reports both faithfully; the eviction gap is its own question.
