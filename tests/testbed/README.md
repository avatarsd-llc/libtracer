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
    a ──dial──▶ hub ◀──dial── b                    two peers on ONE peer_named listener

    172.28.0.11 a    172.28.0.12 b    172.28.0.13 c    172.28.0.14 hub
```

| Node | Link listener | Dials (created remotely by the driver) |
| --- | --- | --- |
| `a` | `c` on :47311 | `b` → b:47311, `hub` → hub:47320 |
| `b` | `a` on :47311 | `c` → c:47311, `hub` → hub:47320 |
| `c` | `b` on :47311 | `a` → a:47311 |
| `hub` | `mesh` on :47320 (**peer_named**) | — |

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
export LIBTRACER_MESH_CTRL="a=127.0.0.1:47301,b=127.0.0.1:47302,c=127.0.0.1:47303,hub=127.0.0.1:47304"
export LIBTRACER_MESH_PEERS="a=172.28.0.11:47311,b=172.28.0.12:47311,c=172.28.0.13:47311,hub=172.28.0.14:47320"
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
core/build/tests/mesh_node --name hub --ctrl-port 47404 --peer-named-listen mesh:47420 --timeout-ms 300000 &
# then point LIBTRACER_MESH_CTRL / _PEERS at 127.0.0.1 with those ports.
```

## Addressing: `/b/c/...`, **not** `/net/b/net/c/...`

A connection's **routing key is its bare name**; its `/net/<name>` key is the *vertex*.
`transport_vertex.cpp` registers the router child under the bare `name` while the graph
composes the vertex at `/net/<name>`, and `fwd_router_t` resolves a FWD's first `dst`
segment against the child-link registry **before** the local graph.

- `/net/b` — where you **read** the link's state, settings, `await` its bring-up
- `b` — the first `dst` segment that **routes through** it

So `a` reaches `c` through `b` at **`/b/c/sensor/temp`**. This is deliberate: #373 exists
precisely *because* link names share the top-level namespace with first-level vertices,
which is only true under bare-name routing.

**[reference/03](../../docs/reference/03-addressing.md) and
[reference/07](../../docs/reference/07-host-embedding.md) are stale here**, and the
conformance vectors encode a *third* form — tracked in **#419**. The driver pins the
implementation and asserts the documented form does **not** resolve, so the docs cannot
quietly become true without this going red.

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

Documented rather than blocked on. Each is a `todo` in the driver asserting *current*
behaviour, so the day the gap closes the test goes red and forces the update.

### 1. Node identity — #406 / RFC-0011

**`/node/name` is a seeded application value, not an identity.** A node can claim anything,
and nothing binds the claim to the device. There is no identity surface at all today:
`peer_id_t` (`transport.hpp`) has **zero call sites**, and RFC-0011's `:identity` facet is
unmerged.

The ring makes the consequence concrete: **`b` reached as `/b` and as `/c/a/b` is two
unrelated paths**, and libtracer will never tell you they are one device — ADR-0044 pt 2,
by design, at any layer, on any node. So the whole-network graph stays a client-side
projection, and a recursive walk cannot terminate on the cycle by itself. Dedup is the
client's job, keyed by an identity **it** chooses (#409's stitch utility); #406 is the
keystone that makes that key trustworthy rather than a guess.

> Note also that `reference/07` describes a 128-bit `peer_id_t` with generation rules. **No
> such thing exists in code.** Reconcile in #406.

### 2. Teardown and link lifecycle — #407 / #66

- **Link state never falls.** `set_link_state(name, true)` is called exactly once, at
  creation. Kill container `c` and `/net/c` on `b` still reads **up** forever.
- **No reconnect anywhere.** A torn dial exits and stays dead.
- **No child removal**, so a link cannot even be recreated under the same name after a
  failure (`PATH_IN_USE`). Recovery needs a **new name** — a hard blocker for
  stable-identity reconnection, and the sharpest argument for #407.
- **`close_peer` has no in-band surface.** #418 made the documented
  `link_of(name)->bus()->close_peer(peer)` path *reachable* (the hub's listener is now both
  config-constructed and `peer_named`), but invoking it needs the removal model #407 owns.

### 3. The revisit error is fiction — #420

`reference/03` and `/07` promise `ERROR{tr::path::invalid}` when a `dst` revisits a node.
**No visited-set, no hop counter, no check exists** — and a stateless forwarder cannot have
one (`fwd_compact_test` *asserts* zero per-request state). `/b/c/a/b/c/a/node/name` orbits
the cycle twice and succeeds.

Loop-freedom does hold, by a different and stronger mechanism: **`dst` is consumed
monotonically**, so a delivery travels exactly as far as its explicit source route. The
testbed asserts the implementation and records the contradiction rather than encoding the
docs' fiction.

## Layout

The harness is C++ and lives with the helper binaries it resembles
(`core/tests/mesh_node.cpp`, beside `ws_interop_server` and `fwd_node_server` — a binary the
driver spawns, deliberately **not** an `add_test()`). The orchestration is not C++ and lives
here, beside `tests/conformance/` and `tests/packaging/`. The driver lives with the client
SDK that is its subject.
