# Subscription is consumer-initiated (a client-write into the producer's `:subscribers[]`); the target stays control-passive

Status: accepted

[ADR-0006](0006-read-write-await-api-no-connect.md) fixed the wire API at `read`/`write`/`await` with **no `connect`/`subscribe` primitive**, and [CONTEXT.md](../../CONTEXT.md) (*SUBSCRIBER direction*) fixed **producer-holds**: the edge lives on the source's `:subscribers[]`, delivery is an ordinary write, and the target is *subscription-unaware* at runtime. Designing third-party network formation forced an unanswered question: **who issues the subscribe-write, and how is a sink protected from unwanted fan-in?** Two readings were live — an orchestrator (or anyone with the source's subscribe-right) writes the edge directly into the producer, versus the *consumer* driving it. This ADR fixes the direction without disturbing producer-holds or the wire API.

## Decision

**A subscription is initiated by the consumer acting as a *client*, exactly like an HTTP/REST request.** The consumer issues one `write` into the producer's standard `:subscribers[]` field — `write(/A/sensor:subscribers[] += SUBSCRIBER{target = /B/in})` — and then holds **nothing**: no source list, no control plane. Its identity is its `origin_peer_id`, the way a REST client is identified by its token rather than by an address. The producer's `:acl` accepts or rejects the request; on accept the producer holds the edge and fans out. The mapping is precise:

| REST | libtracer subscription |
| --- | --- |
| client initiates, has no endpoint, identified by token | **consumer** initiates, holds no source field, identified by `origin_peer_id` |
| server endpoint is addressable, reads the request, may reject | **producer's `:subscribers[]`** is addressable, the producer's `:acl` accepts/rejects |
| response body delivered to the client | delivery written to the consumer's data endpoint |

Three rules follow:

1. **The target is control-*passive*, not data-*dumb*.** The consumer's data endpoint never grows a `:sources[]` plane — it only *receives writes* (load-bearing claim 2 intact). But on the **data** axis it is the opposite of trivial: it holds or streams, carries variable-length structured TLVs and **ropes**, and scales to a GB RTSP frame group via advertise+id-match. "Control-passive, data-rich" — the two axes are independent.

2. **Fan-in and fan-out are governed by the two endpoints' ordinary ACLs — no new machinery.** *Fan-out / confidentiality* is the **producer's `:acl`** ("who may subscribe to me"). *Fan-in / sink protection* is the **consumer's `:acl` on the target** ("who may write to me") — because delivery *is* a write, an unwanted source is rejected at the consumer at delivery time, plus trivial firmware arity logic ("I already have one source `peer_id`; reject a second"). This enforces "multiple publishers will not feed a single sink" **device-locally, with no orchestrator present** — the REST-server-auth shape (the server accepts the connection, then `403`s the request).

3. **`connect(to=…)` is SDK/binding sugar, not a wire verb.** A client-side `connect` expands into the field-write above (and the transport-link field-write of [ADR-0027](0027-transport-and-connections-are-vertices.md)). The wire stays `read`/`write`/`await` — [ADR-0006](0006-read-write-await-api-no-connect.md) holds.

**The three origins of a subscription collapse to one operation.** Firmware-baked sources, NVS-restored sources, and orchestrator-written sources are all *the same client-write into a producer's `:subscribers[]`*, issued by a different driver (firmware on boot, flash restore, or a third party with delegated rights). There is no privileged "default binding" — only a subscribe-write.

## Considered options

- **Orchestrator writes the producer directly; consumer holds nothing and stays passive.** This is in fact the adopted mechanism *for the edge* — but framed as *consumer-as-client*, which is what makes firmware self-reconnect and third-party provisioning the identical operation, and what locates fan-in defense in the consumer's own ACL.
- **A `:sources[]` desired-source field on the consumer (the dual of `:subscribers[]`).** Rejected: it makes the subscriber store its own sources (which [CONTEXT.md](../../CONTEXT.md) explicitly avoids), adds a second control plane and a reconcile surface, and is unnecessary — a control-passive target with firmware/NVS *config* driving the client-write achieves the same self-reconnect without a wire field.
- **Full consumer-pull: move the active edge onto the consumer.** Rejected: abandons producer-holds fan-out efficiency and breaks load-bearing claim 2 (delivery-is-a-write / target subscription-unaware).

## Consequences

- **Producer-holds and the wire API are untouched.** The only thing fixed is *initiation direction* and *where each protection lives*; the glossary's "_Avoid_: the subscriber stores its own sources" stays true because the consumer stores nothing.
- **Sink protection is device-local and orchestrator-independent** — a rebooted leaf re-establishes its sources from firmware/NVS config by re-issuing the client-write; a single-input sink rejects a second writer via its own ACL.
- **Rejection is at delivery time on the consumer, not at bind time on the producer** (the producer accepts the SUBSCRIBER; the consumer drops an unauthorized write). This is the accepted REST trade-off and keeps the producer from needing to know each target's policy.
- The default **transport direction** that pairs with this (consumer dials, producer pushes) is decided in [ADR-0027](0027-transport-and-connections-are-vertices.md). The end-to-end flow is written up in [reference/13](../reference/13-network-formation.md).
