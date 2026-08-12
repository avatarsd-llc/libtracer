# The decentralized mesh testbed (#408)

Four real libtracer nodes, each in its own container with its own network namespace and
IP, wired into a **cycle-containing** topology **the real way** — then routed across.

This is the first place libtracer is exercised as a *network* rather than as a pair of
nodes. It exists because the d2d-hardening milestone (grill 2026-07-17) put one thing
first: **prove decentralized graph formation on libtracer alone, before the originating
production firmware (an ESP32-C6 smart-agriculture node) wires device-to-device.**

## The topology

```
    driver (host, TS client SDK)  ──ws──▶  ctrl:47300 on every node
                                           (published as 47301..47304)

    a ──dial──▶ b ──dial──▶ c ──dial──▶ a          the ring closes: a physical CYCLE
    a ──dial──▶ bus ◀──dial── b                    two peers on ONE peer_named listener

    172.28.0.11 a    172.28.0.12 b    172.28.0.13 c    172.28.0.14 bus
```

| Node | Link listener | Dials (created remotely by the driver) |
| --- | --- | --- |
| `a` | `c` on :47311 | `b` → b:47311, `bus` → bus:47320 |
| `b` | `a` on :47311 | `c` → c:47311, `bus` → bus:47320 |
| `c` | `b` on :47311 | `a` → a:47311 |
| `bus` | `mesh` on :47320 (**peer_named**) | — |

The fourth node is called **`bus`** after the only thing that distinguishes it: it is the
one node whose listener is `peer_named`, so both dialers land on a single connection it
serves as a bus. It is emphatically *not* a hub, master or coordinator — every node here
is an equal peer, and `bus` carries no more traffic and no more authority than `a`, `b` or
`c`. See **Peer / peer symmetry** in [CONTEXT.md](../../CONTEXT.md).

**Naming rule: every node names every link after the node at the FAR end.** That is not
cosmetic — it is what makes replies work. Each forwarder prepends *its own* name for the
**arrival** link to `src`, so a reply retraces the request hop by hop, and the terminus
answers over the link the request arrived on (RFC-0004 §B).

## What forms the mesh — and what doesn't

**The containers do not form the mesh.** Each node creates only its own *listeners* (from
argv, via a local `graph.write` of a SPEC — the identical code path an inbound `FWD{WRITE}`
takes) and then waits.

**Every inter-node link is dialled by the driver, remotely**, by writing a `SPEC` into that
node's `/net:children[]` over the wire. That is the whole point: it exercises the in-band
formation plane a web UI uses (ADR-0017 / ADR-0027, [reference/13](../../docs/reference/13-network-formation.md) §2)
— a third party holding delegated admin creates links on devices and departs, leaving the
devices talking with nothing in the data path. **No `provide_link` seam is used anywhere**;
every link is a real ws socket the built-in `ws` factory constructs from a SPEC's config.

## Running it

```bash
docker compose -f tests/testbed/compose.yml up -d --build --wait

cd bindings/typescript && npm ci && npm run build && cd packages/client
export LIBTRACER_MESH_CTRL="a=127.0.0.1:47301,b=127.0.0.1:47302,c=127.0.0.1:47303,bus=127.0.0.1:47304"
export LIBTRACER_MESH_PEERS="a=172.28.0.11:47311,b=172.28.0.12:47311,c=172.28.0.13:47311,bus=172.28.0.14:47320"
node --test test/mesh-testbed.test.mjs

docker compose -f tests/testbed/compose.yml down -v
```

The driver is env-driven, so the same test also runs against four **local processes** — no
Docker — which is much faster to debug against:

```bash
cmake --build core/build --target mesh_node -j
core/build/tests/mesh_node --name a   --ctrl-port 47401 --listen c:47411 --timeout-ms 300000 &
core/build/tests/mesh_node --name b   --ctrl-port 47402 --listen a:47412 --timeout-ms 300000 &
core/build/tests/mesh_node --name c   --ctrl-port 47403 --listen b:47413 --timeout-ms 300000 &
core/build/tests/mesh_node --name bus --ctrl-port 47404 --peer-named-listen mesh:47420 --timeout-ms 300000 &
# then point LIBTRACER_MESH_CTRL / _PEERS at 127.0.0.1 with those ports.
```

## Addressing: the routing key **is** the vertex path

A connection's routing key is its **full vertex path** — `/net/<module>/<name>` — so the
address you read the link at and the address you route *through* it are the same string
(RFC-0014 S2a, [ADR-0061](../../docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)).
`transport_vertex.cpp` registers the router child under that **qualified** key
(`router_.add_child(qualified, *link)`), and `fwd_router_t` matches a FWD's **leading**
`dst` segments against the child registry, longest mount first — not its first segment.

- `/net/ws-client/b` — where you **read** the link's state, settings, `await` its bring-up
- `/net/ws-client/b` — and the same run a `dst` **routes through**

So `a` reaches `c` through `b` at **`/net/ws-client/b/net/ws-client/c/sensor/temp`**, which
is what `via('b', 'c')` builds in the driver.

> **Erratum (2026-07-31).** This section previously said the opposite — *"a connection's
> routing key is its bare name"*, `/b/c/sensor/temp`, and that
> [reference/03](../../docs/reference/03-addressing.md) and
> [reference/07](../../docs/reference/07-host-embedding.md) *"are stale here"*. Every part of
> that was inverted by RFC-0014 S2a, including the direction of the staleness: the reference
> chapters describe the shipped model and this file did not.
>
> It also claimed *"the driver pins the implementation and asserts the documented form does
> not resolve, so the docs cannot quietly become true without this going red."* The driver
> does pin the implementation — `mesh-testbed.test.mjs:24-25` states the qualified rule and
> `:222` tests `/net/ws-client/b/node/name`. It pinned the **new** form while this file kept
> describing the old one, so the guard was green and the doc was wrong at the same time. A
> guard only protects the claim it actually checks.
>
> #373's rationale went with it: link names no longer share the top-level namespace with
> first-level vertices, because a connection is addressed under `/net`, never at the root.

The conformance vectors still encode a *third* form — that part stands, and is tracked in
**[#419](https://github.com/avatarsd-llc/libtracer/issues/419)**.

## Why no healthchecks

Readiness is **structural**. A built-in DIAL is synchronous with no retry
(`transport_tcp.hpp`: *"Reconnect is out of scope"*), and a refused dial returns before
`register_vertex_key` — leaving **no vertex**, so the whole SPEC must be re-issued.

So each node binds its **link listeners first and its ctrl listener last**. Because ctrl is
the only way the driver can reach a node at all, **ctrl-reachable implies every link
listener on that node is already bound.** The driver connects to all four (retrying) and
only then dials. `depends_on` waits for container *start*, not for a bound socket, and would
add nothing.

## Known absences (xfail)

Documented rather than blocked on.

> **This register has no forcing function, and used to claim one** ([#606]). The text here
> read *"Each is a `todo` in the driver asserting current behaviour, so the day the gap
> closes the test goes red and forces the update."* Every `todo` in the driver has a body
> made **entirely of comments** — no assertions — so none of them can ever go red. The
> mechanism was asserted, never built.
>
> It had already failed silently, in the most visible way available: entry 1 below
> described an identity gap that **#406 and #409 closed**, while `compose.yml` **in this
> same directory** gives every node a `--identity` and the CI job's #409 walk step depends
> on that working. Entry 3 described a docs contradiction that **#420 fixed**, so the
> register outlived the thing it was registering. Both are retired below.
>
> Promoting an entry to a real assertion of current behaviour is the fix. It needs the
> compose stack, so it belongs to the `mesh-testbed` CI job.

[#606]: https://github.com/avatarsd-llc/libtracer/issues/606

### 1. ~~Node identity~~ — RETIRED, #406 / #409 closed

This entry claimed *"There is no identity surface at all today: `peer_id_t` has zero call
sites, and RFC-0011's `:identity` facet is unmerged."* Both clauses are false:
`graph_t::set_identity` / `clear_identity` / `read_identity` implement the RFC-0011 §B kind
registry, and `compose.yml` gives each node a distinct `--identity` precisely so the #409
walk collapses the ring into 4 devices instead of unrolling it into ~170 route-nodes.

**What is still true is a design property, not an absence.** `b` reached as `/b` and as
`/c/a/b` is two unrelated *paths*, and libtracer will never tell you they are one device —
ADR-0044 pt 2, by design, at any layer, on any node. So the whole-network graph stays a
client-side projection and dedup is the client's job, keyed by the identity facet. That is
the architecture working as intended; it does not belong in a register of gaps.

> One phantom outlives #406: `reference/07` describes a 128-bit `peer_id_t` with generation
> rules, while `transport.hpp:36` declares `using peer_id_t = std::array<std::byte, 16>`
> with no generation rules. #406 closed without reconciling it, and it is covered by neither
> #599 nor #586 — tracked in [#606].

### 2. Teardown and link lifecycle — #407 / #66 (narrowed)

- ~~**No reconnect anywhere.**~~ **Landed.** RFC-0014 §4 gave `transport_vertex.hpp` a
  six-state `link_state_t` (`transport_vertex.hpp:97`) including `RECONNECTING`, plus
  `backoff_ms` (`transport_vertex.hpp:140`) and `connect_timeout_ms` (`transport_vertex.hpp:143`).
- **No child removal**, so a link cannot be recreated under the same name after a failure
  (`PATH_IN_USE`). Recovery needs a **new name** — a hard blocker for stable-identity
  reconnection, and the sharpest argument for #407.
- **`close_peer` has no in-band surface.** #418 made the documented
  `link_of(name)->bus()->close_peer(peer)` path *reachable* (the bus's listener is now both
  config-constructed and `peer_named`), but invoking it needs the removal model #407 owns.

### 3. ~~The revisit error is fiction~~ — RETIRED, #420 closed the docs side

This entry claimed *"`reference/03` and `/07` promise `ERROR{tr::path::invalid}` when a
`dst` revisits a node."* They no longer do — #420 corrected them, and
`reference/07-host-embedding.md:79` now states the opposite outright: *"There is no revisit
check, no visited-set, and no error status for a route that re-enters a node."* `:285` makes
it a rule an implementation must not "fix".

The behaviour the entry described is unchanged and correct — loop-freedom holds by
**monotonic `dst` consumption**, so a delivery travels exactly as far as its explicit source
route, and `/b/c/a/b/c/a/node/name` orbits the cycle twice and succeeds. There is simply no
longer a contradiction to register.

## Layout

The harness is C++ and lives with the helper binaries it resembles
(`core/tests/mesh_node.cpp`, beside `ws_interop_server` and `fwd_node_server` — a binary the
driver spawns, deliberately **not** an `add_test()`). The orchestration is not C++ and lives
here, beside `tests/conformance/` and `tests/packaging/`. The driver lives with the client
SDK that is its subject.
