# A remote AWAIT completes from a one-shot receiver-side waiter charged to the receiving link, and never holds that link's receive context

<!-- status: accepted -->

Status: **accepted** (2026-10-03, maintainer-directed). Closes the deferred-reply-completion follow-on recorded in [ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) ("Probe-on-demand ... needs deferred/await-style reply completion at the terminus first"). Composes with [ADR-0006](0006-read-write-await-api-no-connect.md) (the read/write/await data API), [ADR-0067](0067-bounded-recycling-source-and-per-owner-topology.md) §3 (each link's own rx source) and the no-library-internal-buffer rule ([`CONTEXT.md`](../../CONTEXT.md) §Resource bound).

## Context

Until this ADR, the terminus resolved every request synchronously on the thread that delivered it. For READ and WRITE that is the right shape: both finish in a bounded number of steps. AWAIT is different. `apply_op`'s AWAIT arm called `graph_t::await(v, timeout, subject)`, which blocks on the vertex stripe's condition variable until the vertex changes or the deadline passes. The deadline is the request's `await_timeout` child, or `kDefaultAwaitTimeout` (1 s) when it has none, and a peer chooses it.

The thread that blocked was the receive context of the link the AWAIT arrived on. For that whole time the link processed no later frame: a READ sent right after the AWAIT on the same link waited behind it. On a transport whose one receive thread serves several links, every link on that thread waited too.

The terminus had no way to answer later. `op_resolver_t::resolve` returns the reply rope, and the router sends that rope on the inbound link before the receive call returns. Nothing kept the reply's route once the request frame was released.

## Decision

**1. A remote AWAIT registers a one-shot waiter on the vertex and the receive context returns at once.** The terminus gates the READ right first (a denied caller still gets `PERMISSION_DENIED` at once and parks nothing), then hands the request to a deferral sink and sends nothing. The reply goes out **on change**, from the writer's thread, right after the publish lands. It is the same value `graph_t::await` would have returned, through the same role dispatch (`graph_t::await_value`, now shared by both forms).

A pending waiter that sees no change ends when its link goes down (`link_down`, and `remove_child` through it) or when the router is destroyed. Each AWAIT is answered at most once. A publish and a cancel race under the vertex stripe lock, and whichever unlinks the waiter first owns it.

**1a. The deadline is open (TODO).** libtracer has no timers (RFC-0005's ban, restated in RFC-0025 §4.1.3): the receiver neither runs a timer thread nor reads a clock. Today the request's `await_timeout` is carried to the sink (`deferred_await_t::timeout`) and not enforced. Two ways to close this are under maintainer review: the requester alone owns the timeout and the receiver holds a deadline-free waiter, or the application drives receiver deadlines through one router call that is passed its own clock. The code marks the spot `TODO(ADR-0084)`.

**2. The receiver pays, and no library-internal buffer is added.** The waiter and everything its reply needs (the link name, the swapped route bytes, the wire-time echo and the RFC-0024 mint) are ONE block drawn from the receiving link's own rx source (`fwd_router_t::rx_for`, ADR-0067 §3), the same source that link's decode arena draws from. A peer that parks AWAITs spends its own link's budget and nobody else's. When that source refuses the block, this AWAIT is answered `tr::flow::backpressure` at once. It never falls back to the blocking wait. The block goes back to the source when the reply is sent, when the link goes down (`link_down`, and `remove_child` through it) and when the router is destroyed.

**3. The graph gets an intrusive waiter, not a callback table.** `await_waiter_t` is a caller-owned `{fire, ctx}` record that `vertex_t::arm_waiter` links into the vertex's lock stripe. An armed waiter is counted in the stripe's existing `waiters` count, so a publish on a stripe with no waiter of either kind keeps #555's lock-free path unchanged. The slow path, which already took the stripe mutex to notify, now also unlinks this vertex's armed waiters and fires them after it releases the mutex. `graph_t::arm_await` / `disarm_await` / `await_value` are the public face. The blocking `graph_t::await` is unchanged for local callers.

**4. The resolver defers only when its caller can answer later.** `op_resolver_t::on_await_defer` installs the sink, and `resolve` defers only when it is also given a `deferred` out-flag. A direct `resolve` with no out-flag (an embedder's own sink, the resolver unit tests) keeps the synchronous AWAIT it always had. The router installs the sink and passes the flag at both termini (arena and rope tier).

**5. The reply takes the request's own canonical return route.** The deferred reply is the identical `FWD{REPLY}` the synchronous arm would have sent, built by the same `assemble_reply` / `assemble_error_reply`, with the same `dst` (the request's accumulated `src`), the same label-substituted `src` on a RESULT and the same echo. Nothing of RFC-0029 S5 (the chain-first delivery leg) is used or needed: a reply already carries its whole way home, so this ADR implements no RFC-0029 slice and takes no S5 design. When S5 lands, nothing here changes.

## Consequences

**Reply order on a link changes, and the spec already allows it.** A READ sent after a pending AWAIT on the same link is now answered first. [`reference/04`](../reference/04-communication-flows.md) §Invariants already says replies on one link carry no ordering promise and that an origin correlates a reply by its `src` suffix, not by arrival order. The deferred AWAIT is the first terminus-local case that relies on that, so the cost lands on one pattern: an origin with an AWAIT and a READ outstanding **to the same vertex** on one link cannot tell the two replies apart by suffix. The documented fallback (the oldest outstanding match) then hands the READ's answer to the AWAIT. An origin that needs both should not pipeline a READ behind an AWAIT on the same vertex, or should await through a distinct reply route. This is a client-side consequence and not a wire change: `v1.md` makes no ordering promise to break. Pairing by suffix plus op is not possible without a wire change: a `FWD{REPLY}` carries `op = REPLY` (bits 7–6 reserved, MUST be zero), `dst`, `src`, `kind` and the payload, and nothing that names the request's op. A RESULT to an AWAIT and a RESULT to a READ of the same vertex are byte-identical. Telling them apart on the wire would be a spec amendment, which this ADR does not make.

**No thread and no clock.** The fix adds no thread, and the router reads no clock. Until the deadline ruling (1a) lands, a remote AWAIT on a vertex that never changes is not answered by the receiver. The requester's own timeout is what ends it on that side, and the waiter block stays charged to the link until the link goes down or the router is destroyed.

**The writer's thread sends the change reply.** Like `deliver_remote` for a subscription, a write to an awaited vertex builds and sends one reply per armed waiter, after the publish and outside every graph lock. A local write that wakes N remote AWAITs pays N sends.

**A parked AWAIT costs memory, not a thread.** Before, an AWAIT held a receive thread for its whole deadline. Now it holds one waiter block of its own link's rx source, which the embedder bounds by injecting a bounded source. When that source is exhausted, the next AWAIT is answered `tr::flow::backpressure`: the receiver pays, and only on the link that asked.

**Cost on the hot paths.** The waiterless publish is unchanged: one `seq_cst` load of the stripe count, as before. A publish on a stripe that has waiters walks that stripe's armed list under the mutex it already held. The terminus READ/WRITE path gains one `bool` on the stack and one predictable branch. `vertex_stripe_t` grows by one pointer, inside its existing cache-line padding on the host.

## Alternatives considered

- **Keep the blocking wait and cap the timeout.** A cap shortens the stall but keeps it: a peer still holds the link (and every link on its thread) for the cap, as often as it likes.
- **Run each AWAIT on its own thread.** That is a thread, a stack and a spawn per request a peer chooses to send. It turns the stall into a resource exhaustion that the receiving link's source cannot bound.
- **A router-owned timer thread for deadlines (rejected).** It was the first cut of this fix: one lazily started thread per router, sleeping until the earliest deadline and answering `tr::flow::timeout`. It is rejected because libtracer contains no timers of its own. A library thread that wakes on a clock is a timer, whatever starts it, and the narrow targets this library serves would pay its stack on every node.
- **A compile-time deadline policy (timer thread on hosts, app-driven on narrow targets).** Rejected for the same reason: the host default would still be a library timer.
- **Answer timeouts lazily (on the next frame or the next write).** An idle link would never see its timeout, and checking it needs a clock read.
- **An application-driven `expire_awaits(now)` call.** It needs no thread and reads no clock, because the app passes its own time. It is one of the two options under review (1a), not rejected.
- **A subscription edge as the waiter.** A one-shot `:subscribers[]` edge would reuse fan-out, but a delivery is a `FWD{WRITE}` to a subscriber target, not the `FWD{REPLY}` the origin is waiting for. It would also touch the edge layout #1533 and RFC-0029 S5 settle.
