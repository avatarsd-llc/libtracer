# A remote AWAIT completes from a one-shot receiver-side waiter charged to the receiving link, and never holds that link's receive context

<!-- status: accepted -->

Status: **accepted** (2026-10-03, maintainer-directed). Closes the deferred-reply-completion follow-on recorded in [ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) ("Probe-on-demand ... needs deferred/await-style reply completion at the terminus first"). Composes with [ADR-0006](0006-read-write-await-api-no-connect.md) (the read/write/await data API), [ADR-0067](0067-bounded-recycling-source-and-per-owner-topology.md) §3 (each link's own rx source) and the no-library-internal-buffer rule ([`CONTEXT.md`](../../CONTEXT.md) §Resource bound).

## Context

Until this ADR, the terminus resolved every request synchronously on the thread that delivered it. For READ and WRITE that is the right shape: both finish in a bounded number of steps. AWAIT is different. `apply_op`'s AWAIT arm called `graph_t::await(v, timeout, subject)`, which blocks on the vertex stripe's condition variable until the vertex changes or the deadline passes. The deadline is the request's `await_timeout` child, or `kDefaultAwaitTimeout` (1 s) when it has none, and a peer chooses it.

The thread that blocked was the receive context of the link the AWAIT arrived on. For that whole time the link processed no later frame: a READ sent right after the AWAIT on the same link waited behind it. On a transport whose one receive thread serves several links, every link on that thread waited too.

The terminus had no way to answer later. `op_resolver_t::resolve` returns the reply rope, and the router sends that rope on the inbound link before the receive call returns. Nothing kept the reply's route once the request frame was released.

## Decision

**1. A remote AWAIT registers a one-shot waiter on the vertex and the receive context returns at once.** The terminus gates the READ right first (a denied caller still gets `PERMISSION_DENIED` at once and parks nothing), then hands the request to a deferral sink and sends nothing. The reply goes out later, from one of two places:

- **on change**, from the writer's thread, right after the publish lands. It is the same value `graph_t::await` would have returned, through the same role dispatch (`graph_t::await_value`, now shared by both forms);
- **on timeout**, from a router-owned timer thread, as the addressed `tr::flow::timeout` the blocking form returned.

Each AWAIT is answered exactly once. Which answer wins is decided under the vertex stripe lock: a publish that unlinks the waiter fires it, and a timer or a teardown that unlinks it first owns it.

**2. The receiver pays, and no library-internal buffer is added.** The waiter and everything its reply needs (the link name, the swapped route bytes, the wire-time echo and the RFC-0024 mint) are ONE block drawn from the receiving link's own rx source (`fwd_router_t::rx_for`, ADR-0067 §3), the same source that link's decode arena draws from. A peer that parks AWAITs spends its own link's budget and nobody else's. When that source refuses the block, this AWAIT is answered `tr::flow::backpressure` at once. It never falls back to the blocking wait. The block goes back to the source when the reply is sent, when the link goes down (`link_down`, and `remove_child` through it) and when the router is destroyed.

**3. The graph gets an intrusive waiter, not a callback table.** `await_waiter_t` is a caller-owned `{fire, ctx}` record that `vertex_t::arm_waiter` links into the vertex's lock stripe. An armed waiter is counted in the stripe's existing `waiters` count, so a publish on a stripe with no waiter of either kind keeps #555's lock-free path unchanged. The slow path, which already took the stripe mutex to notify, now also unlinks this vertex's armed waiters and fires them after it releases the mutex. `graph_t::arm_await` / `disarm_await` / `await_value` are the public face. The blocking `graph_t::await` is unchanged for local callers.

**4. The resolver defers only when its caller can answer later.** `op_resolver_t::on_await_defer` installs the sink, and `resolve` defers only when it is also given a `deferred` out-flag. A direct `resolve` with no out-flag (an embedder's own sink, the resolver unit tests) keeps the synchronous AWAIT it always had. The router installs the sink and passes the flag at both termini (arena and rope tier).

**5. The reply takes the request's own canonical return route.** The deferred reply is the identical `FWD{REPLY}` the synchronous arm would have sent, built by the same `assemble_reply` / `assemble_error_reply`, with the same `dst` (the request's accumulated `src`), the same label-substituted `src` on a RESULT and the same echo. Nothing of RFC-0029 S5 (the chain-first delivery leg) is used or needed: a reply already carries its whole way home, so this ADR implements no RFC-0029 slice and takes no S5 design. When S5 lands, nothing here changes.

## Consequences

**Reply order on a link changes, and the spec already allows it.** A READ sent after a pending AWAIT on the same link is now answered first. [`reference/04`](../reference/04-communication-flows.md) §Invariants already says replies on one link carry no ordering promise and that an origin correlates a reply by its `src` suffix, not by arrival order. The deferred AWAIT is the first terminus-local case that relies on that, so the cost lands on one pattern: an origin with an AWAIT and a READ outstanding **to the same vertex** on one link cannot tell the two replies apart by suffix. The documented fallback (the oldest outstanding match) then hands the READ's answer to the AWAIT. An origin that needs both should not pipeline a READ behind an AWAIT on the same vertex, or should await through a distinct reply route. This is a client-side consequence and not a wire change: `v1.md` makes no ordering promise to break.

**One timer thread per router, started lazily.** It starts on the first deferred AWAIT, so a node that serves none never has it. It is spawned with `pthread_create` and not `std::thread`, so a failed spawn refuses that one AWAIT (`backpressure`) instead of aborting under `-fno-exceptions` (the `self_heal_link_t` precedent, #1470). It sleeps until the earliest deadline and holds no buffer. It uses the platform's default stack. A stack-size knob in the shape of `kSelfHealWorkerStackBytes` is a follow-on if a narrow target needs one.

**The writer's thread sends the change reply.** Like `deliver_remote` for a subscription, a write to an awaited vertex builds and sends one reply per armed waiter, after the publish and outside every graph lock. A local write that wakes N remote AWAITs pays N sends.

**A peer-chosen deadline now costs memory, not a thread.** Before, an AWAIT with a huge `await_timeout` held a receive thread for that long. Now it holds one waiter block of its own link's rx source, which the embedder bounds by injecting a bounded source. A per-link cap on the deadline is not added here.

**Cost on the hot paths.** The waiterless publish is unchanged: one `seq_cst` load of the stripe count, as before. A publish on a stripe that has waiters walks that stripe's armed list under the mutex it already held. The terminus READ/WRITE path gains one `bool` on the stack and one predictable branch. `vertex_stripe_t` grows by one pointer, inside its existing cache-line padding on the host.

## Alternatives considered

- **Keep the blocking wait and cap the timeout.** A cap shortens the stall but keeps it: a peer still holds the link (and every link on its thread) for the cap, as often as it likes.
- **Run each AWAIT on its own thread.** That is a thread, a stack and a spawn per request a peer chooses to send. It turns the stall into a resource exhaustion that the receiving link's source cannot bound.
- **Answer timeouts lazily (on the next frame or the next write).** An idle link would never see its timeout. The acceptance criterion is that the timeout reply arrives.
- **An application-driven `service()` call for deadlines.** This needs no thread, but it makes every embedder add a call whose omission silently turns every remote AWAIT into one that never times out. It stays possible later as a compile-time policy beside the thread if a target wants no thread.
- **A subscription edge as the waiter.** A one-shot `:subscribers[]` edge would reuse fan-out, but a delivery is a `FWD{WRITE}` to a subscriber target, not the `FWD{REPLY}` the origin is waiting for. It would also touch the edge layout #1533 and RFC-0029 S5 settle.
