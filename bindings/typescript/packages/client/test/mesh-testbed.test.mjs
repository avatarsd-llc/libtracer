// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief #408 — the decentralized MESH testbed driver: form an arbitrary,
 * cycle-containing multi-node topology the REAL way, then route across it.
 *
 * The driver is a third libtracer node (the TS client SDK) wearing the ORCHESTRATING
 * hat (reference/13): it holds a ctrl link to each device, DIALS every inter-node link
 * by writing a `SPEC` to that device's `/net/ws-client/conn` creator endpoint **remotely**,
 * and then routes
 * through the mesh it just made. That is the web-ui-as-setup-edge story executed
 * end to end — no `provide_link`, no config file, no test seam: every link is a real ws
 * socket the built-in `ws` factory constructs from a SPEC's config.
 *
 * TOPOLOGY — a 3-node ring (the deliberate physical CYCLE) plus a peer-enumeration bus:
 *
 *      a ──dial──▶ b ──dial──▶ c ──dial──▶ a        (the ring closes: a physical cycle)
 *      a ──dial──▶ bus ◀──dial── b                  (two peers on ONE peer_named listener)
 *
 * NAMING RULE: every node names every link after the node at the FAR end. That is what
 * makes replies retrace: each forwarder prepends its own MOUNT PATH for the ARRIVAL link
 * to `src`, and the terminus answers over the arrival link (RFC-0004 §B).
 *
 * ADDRESSING: a connection's routing key IS its vertex path — `/net/<module>/<name>`, so a
 * two-hop route is `/net/ws-client/b/net/ws-client/c/node/name` (RFC-0014 §1, ADR-0061).
 * This CLOSES #419: reference/03 + /07 were right about the shape and the old bare-name
 * demux was the divergence. The `via()` helper below builds these routes. These assertions
 * are the repo's first two-forwarder coverage (every existing FWD test has exactly one).
 *
 * GUARDED on LIBTRACER_MESH_CTRL / LIBTRACER_MESH_PEERS. Plain `npm test` without them
 * SKIPS gracefully; the `mesh-testbed` CI job brings the compose stack up and sets both.
 * No fixed sleeps — every connect, dial and delivery is awaited behind a deadline.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
import { TransportWs } from '@avatarsd-llc/libtracer-ws';
import { TYPE, decode } from '@avatarsd-llc/libtracer';
import { LibtracerClient, encodeValue, encodeConnSpec, DELIVERY_DURABILITY_REQUEST } from '../dist/index.js';

/** @brief `name=host:port,…` → `{name: {host, port}}`. */
function parseEndpoints(spec) {
  const out = {};
  for (const entry of (spec ?? '').split(',').filter(Boolean)) {
    const [name, hostport] = entry.split('=');
    const idx = hostport.lastIndexOf(':');
    out[name] = { host: hostport.slice(0, idx), port: Number(hostport.slice(idx + 1)) };
  }
  return out;
}

/** @brief Each node's CTRL endpoint, reachable from the driver (published ports in compose). */
const CTRL = parseEndpoints(process.env.LIBTRACER_MESH_CTRL);
/** @brief Each node's LINK-listener endpoint, reachable from INSIDE the mesh (static IPs). */
const PEERS = parseEndpoints(process.env.LIBTRACER_MESH_PEERS);

const NODES = ['a', 'b', 'c', 'bus'];
const skip = !NODES.every((n) => CTRL[n] && PEERS[n]);

/** @brief The `/sensor/temp` value every node seeds — pinned in `core/tests/mesh_node.cpp`. */
const SEEDED_TEMP = 0x1234abcd;
const PUSHED_SAMPLE = 0xcafebabe;

const utf8 = new TextDecoder();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** @brief Little-endian u32 bytes. */
function le32(v) {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, v >>> 0, true);
  return b;
}

/**
 * @brief Connect a client to a node's ctrl link, retrying until the deadline.
 *
 * The retry IS the readiness gate. A node binds its link listeners BEFORE its ctrl
 * listener (`mesh_node.cpp`), so a successful ctrl connect proves every listener on that
 * node is already bound — which is what lets the dials below race nothing. Containers may
 * still be starting, hence the retry rather than a healthcheck.
 */
async function connectCtrl(name, budgetMs = 60000) {
  const { host, port } = CTRL[name];
  const deadline = Date.now() + budgetMs;
  let lastErr;
  while (Date.now() < deadline) {
    const transport = new TransportWs(`ws://${host}:${port}`, { WebSocket });
    try {
      await transport.connect();
      // A multi-hop request crosses two forwarders and (for a subscribe) waits on a
      // producer, so the 10 s default deadline is tight under a cold container start.
      const client = new LibtracerClient(transport, {
        replyEndpoint: ['driver'],
        requestTimeoutMs: 20000,
      });
      // The transport, not the client, owns the socket — hold it so teardown can close it
      // (an open ws keeps the node event loop alive and the test file never exits).
      return { client, transport };
    } catch (err) {
      lastErr = err;
      await transport.close().catch(() => {});
      await sleep(250);
    }
  }
  throw new Error(`ctrl connect to ${name} (${host}:${port}) timed out: ${lastErr}`);
}

/** @brief The DIAL module every node declares (`kWsClientSuggestedModule`, `mesh_node.cpp`). */
const WS_CLIENT_MODULE = 'ws-client';

/**
 * @brief Remotely DIAL a link: write a SPEC to `node`'s `/net/ws-client/conn` endpoint.
 *
 * RFC-0014 S7 (#1492) retired the global `/net:children[]` creation door, so the SPEC now
 * goes to the DIAL module's own creator endpoint as a WHOLE-VERTEX write. That endpoint is
 * what carries the role and the transport, which is why the payload names neither: the
 * connection lands at `/net/ws-client/<linkName>`, the same path the assertions below read.
 *
 * Retries the WHOLE write. Since the RFC-0014 §4 S5 flip (#1548) a built-in DIAL is
 * engine-managed, so this write creates the connection DORMANT and the first frame routed
 * through it auto-wakes the dial — the retry loop now covers a refused WRITE (an unready
 * ctrl peer), not a refused connect. It is kept because before the flip a refused dial
 * returned before `register_vertex_key`, leaving NO vertex, and re-issuing the same SPEC
 * was the only recovery; a build that closes the engine out still behaves that way.
 * `linkName` is the FAR node's name, per the naming rule.
 */
async function dial(client, from, linkName, toNode, budgetMs = 30000) {
  const { host, port } = PEERS[toNode];
  const spec = encodeConnSpec({
    name: linkName,
    port,
    kind: 'ws',
    addr: host,
  });
  const deadline = Date.now() + budgetMs;
  let lastErr;
  while (Date.now() < deadline) {
    try {
      await client.write(['net', WS_CLIENT_MODULE, 'conn'], spec);
      return;
    } catch (err) {
      lastErr = err;
      await sleep(500);
    }
  }
  throw new Error(`dial ${from} -> ${toNode} (as "${linkName}") failed: ${lastErr}`);
}

/** @brief The NAME strings of a synthesized/enumerated `:children[]` POINT listing. */
function listingNames(tlv) {
  const names = [];
  for (const member of tlv.children ?? []) {
    for (const f of member.children ?? []) {
      if (f.type === TYPE.NAME) names.push(utf8.decode(f.payload));
    }
  }
  return names.sort();
}

/**
 * @brief Expand link names into their MOUNT paths (RFC-0014 / ADR-0061).
 *
 * A connection lives at `/net/<module>/<name>` and its routing address IS that path, so a
 * hop through link `b` is the three segments `net`, `ws-client`, `b`. Every link routed
 * through in this testbed is a DIAL created with `kind: 'ws'`, hence the `ws-client`
 * module; the inbound listeners each node binds are `ws-server`.
 */
const via = (...names) => names.flatMap((n) => ['net', 'ws-client', n]);

/** @brief Read `/…/node/name` through `route` and return the terminus node's seeded name. */
async function nameVia(client, route) {
  const tlv = await client.read([...route, 'node', 'name']);
  return utf8.decode(tlv.payload);
}

test('mesh testbed: form a cyclic multi-node mesh in band, then route across it', { skip }, async (t) => {
  // ---- 1) ctrl links to every node (the readiness gate) ------------------------------
  const cli = {};
  const transports = [];
  for (const n of NODES) {
    const { client, transport } = await connectCtrl(n);
    cli[n] = client;
    transports.push(transport);
  }
  t.after(async () => {
    for (const tr of transports) await tr.close().catch(() => {});
  });
  t.diagnostic(`ctrl links up to: ${NODES.join(', ')} — every link listener is bound`);

  // ---- 2) form the mesh: five dials, all issued REMOTELY over ctrl --------------------
  // The ring. Each dial names its link after the far node, so replies retrace.
  await dial(cli.a, 'a', 'b', 'b');
  await dial(cli.b, 'b', 'c', 'c');
  await dial(cli.c, 'c', 'a', 'a'); // closes the CYCLE: a -> b -> c -> a
  // The bus: two peers on ONE peer_named listener (ADR-0044 Brick C).
  await dial(cli.a, 'a', 'bus', 'bus');
  await dial(cli.b, 'b', 'bus', 'bus');
  t.diagnostic('mesh formed: ring a->b->c->a plus a,b->bus — every link an in-band SPEC');

  await t.test('each node reports the connections it was told to create', async () => {
    // Ordinary vertex enumeration (NOT peer enumeration). RFC-0014 §1: /net enumerates the
    // MODULES, and each module enumerates its member connections — so a node's dials and its
    // listeners now list separately, which is the whole point of per-module scoping.
    //
    // A module's listing is its member CONNECTIONS and nothing else: since S4 the `conn`
    // CREATOR ENDPOINT is HIDDEN from it (RFC-0014 §3), because the endpoint is the control
    // that creates connections, not one of them. It stays addressable — §6's creatability
    // probe reads `/net/<module>/conn:schema` — it is simply not a member.
    assert.deepEqual(listingNames(await cli.a.readField(['net'], ':children[]')),
                     ['ws-client', 'ws-server']);
    assert.deepEqual(listingNames(await cli.a.readField(['net', 'ws-client'], ':children[]')),
                     ['b', 'bus']);
    assert.deepEqual(listingNames(await cli.a.readField(['net', 'ws-server'], ':children[]')),
                     ['c', 'ctrl']);
    // The module lists its children in NAME order, not dial order: b dials c first, yet
    // `bus` is listed first. (`hub` sorted after `c`, which is why this line moved.)
    assert.deepEqual(listingNames(await cli.b.readField(['net', 'ws-client'], ':children[]')),
                     ['bus', 'c']);
    assert.deepEqual(listingNames(await cli.b.readField(['net', 'ws-server'], ':children[]')),
                     ['a', 'ctrl']);
    assert.deepEqual(listingNames(await cli.c.readField(['net', 'ws-client'], ':children[]')),
                     ['a']);
    assert.deepEqual(listingNames(await cli.c.readField(['net', 'ws-server'], ':children[]')),
                     ['b', 'ctrl']);
    // The bus only ever listens — but `mesh_node` DECLARES both modules, and since S2b a
    // declared module is minted whether or not it ever carries a connection. A creator has to
    // find the module before it can write the first SPEC to its endpoint, so discoverability
    // lives one level UP: `/net:children[]` names the empty `ws-client` module, and that
    // module's own listing is legitimately EMPTY — no connections, and the endpoint hidden.
    assert.deepEqual(listingNames(await cli.bus.readField(['net'], ':children[]')),
                     ['ws-client', 'ws-server']);
    assert.deepEqual(listingNames(await cli.bus.readField(['net', 'ws-client'], ':children[]')),
                     []);
    assert.deepEqual(listingNames(await cli.bus.readField(['net', 'ws-server'], ':children[]')),
                     ['ctrl', 'mesh']);
  });

  await t.test('a local read resolves at the terminus (no hop)', async () => {
    assert.equal(await nameVia(cli.a, []), 'a');
    assert.equal(await nameVia(cli.c, []), 'c');
  });

  await t.test('ONE hop: /net/ws-client/b/node/name reaches b', async () => {
    assert.equal(await nameVia(cli.a, via('b')), 'b');
  });

  await t.test('TWO hops: /b/c/node/name reaches c through b', async () => {
    // The repo's first two-forwarder assertion. a strips "b" -> forwards to b; b strips
    // "c" -> forwards to c; c resolves /node/name locally and the REPLY retraces
    // c -> b -> a -> driver by the accumulated src.
    assert.equal(await nameVia(cli.a, via('b', 'c')), 'c');
  });

  await t.test('#419 CLOSED: the /net-prefixed address is now the resolving one', async () => {
    // This assertion is INVERTED by RFC-0014 S2a. It used to pin the divergence: the docs
    // promised /net/b/net/c/... while the impl keyed the demux on a bare leaf NAME, so the
    // prefixed form missed the registry and fell through to a local terminus. Routing-address
    // now EQUALS vertex-path (ADR-0061), so the documented form is the real one...
    assert.equal(await nameVia(cli.a, via('b', 'c')), 'c');
    // ...and the old bare-name form no longer routes: "b" is not a first-level vertex, it is
    // a leaf under /net/ws-client, so the dst falls through to this node's local terminus.
    await assert.rejects(() => nameVia(cli.a, ['b', 'c']));
  });

  await t.test('CYCLE: an orbiting dst is not rejected — it terminates by dst exhaustion', async () => {
    // a -> b -> c -> a: a full orbit of the physical cycle, back to the origin node.
    assert.equal(await nameVia(cli.a, via('b', 'c', 'a')), 'a');
    // Two full orbits. There is NO visited-set and NO hop counter anywhere: loop-freedom
    // holds ONLY because dst is consumed monotonically, so a route is as long as it says
    // it is. The ERROR{tr::path::invalid} that reference/03:208 promises on a revisit does
    // not exist and cannot (the forwarder is stateless by design) — see #420.
    assert.equal(await nameVia(cli.a, via('b', 'c', 'a', 'b', 'c', 'a')), 'a');
    // The corollary that matters: a RECURSIVE WALK gets no protection from any of this.
    // Terminating one needs client-side identity-keyed dedup (#406 -> #409).
  });

  await t.test('a remote read through 2 hops returns the byte-exact seeded VALUE', async () => {
    const tlv = await cli.a.read([...via('b', 'c'), 'sensor', 'temp']);
    assert.deepEqual(new Uint8Array(tlv.payload), le32(SEEDED_TEMP));
  });

  await t.test('a remote WRITE through 2 hops lands, and reads back', async () => {
    await cli.a.write([...via('b', 'c'), 'sensor', 'temp'], encodeValue(le32(PUSHED_SAMPLE)));
    const tlv = await cli.a.read([...via('b', 'c'), 'sensor', 'temp']);
    assert.deepEqual(new Uint8Array(tlv.payload), le32(PUSHED_SAMPLE));
    // And the value really moved on the far node, not in a cache on the near one.
    const atC = await cli.c.read(['sensor', 'temp']);
    assert.deepEqual(new Uint8Array(atC.payload), le32(PUSHED_SAMPLE));
  });

  await t.test('SUBSCRIBE through 2 hops: latch + a live write-driven delivery', async () => {
    const seen = [];
    let resolveFirst;
    const first = new Promise((r) => (resolveFirst = r));
    // ValueHandler is (payloadBytes, tlv) — the opaque VALUE payload comes first.
    await cli.a.subscribe(
      [...via('b', 'c'), 'sensor', 'temp'],
      (value) => {
        seen.push(new Uint8Array(value));
        resolveFirst();
      },
      { deliveryPolicy: DELIVERY_DURABILITY_REQUEST },
    );
    // The subscribe REQUESTS durability (RFC-0022 §3.A bit 5), so the producer LATCHES the
    // current value: one delivery with no producer thread. The producer's return route is
    // the ACCUMULATED src, so the delivery retraces c -> b -> a -> driver.
    await first;
    assert.deepEqual(seen[0], le32(PUSHED_SAMPLE), 'the latch delivered the current value');

    // A later write fans out a live delivery over the same 2-hop return route.
    const second = new Promise((r) => {
      const iv = setInterval(() => {
        if (seen.length >= 2) {
          clearInterval(iv);
          r();
        }
      }, 20);
      setTimeout(() => {
        clearInterval(iv);
        r();
      }, 10000);
    });
    await cli.c.write(['sensor', 'temp'], encodeValue(le32(SEEDED_TEMP)));
    await second;
    assert.equal(seen.length >= 2, true, 'a producer write fanned out a delivery across 2 hops');
    assert.deepEqual(seen[1], le32(SEEDED_TEMP));
  });

  await t.test('ADR-0044 Brick C: the bus lists its live peers from real traffic', async () => {
    // /net/mesh is a peer_named ws listener CREATED IN BAND (its peer_named key is
    // ws-private config, parsed by the ws factory — ADR-0043 §5). Both a and b dialled it.
    //
    // Since the RFC-0014 §4 S5 flip (#1548) a built-in DIAL is engine-managed: the SPEC
    // creates the connection DORMANT and the socket is constructed on first use. So the two
    // dials alone put NOTHING on the bus's wire, and the listing this case is about would
    // legitimately be empty. One op through each link is what makes the peers real — which is
    // what "from real traffic" always meant; before the flip a connect happened to supply it.
    assert.equal(await nameVia(cli.a, via('bus')), 'bus');
    assert.equal(await nameVia(cli.b, via('bus')), 'bus');

    const listing = await cli.bus.readField(['net', 'ws-server', 'mesh'], ':children[]');
    const peers = listingNames(listing);
    assert.equal(peers.length, 2, `the bus hears exactly its 2 dialers (got ${JSON.stringify(peers)})`);
    // Peer names are the routable p<slot> fallback (#426, ADR-0073 §2) — a LEGAL path
    // segment, which the old <ip>:<port> spelling never was. Slot order is arrival-
    // dependent, so assert the shape and distinctness, never a literal.
    for (const p of peers) assert.match(p, /^p\d+$/);
    assert.equal(new Set(peers).size, 2, 'the two dialers carry two distinct slot names');

    // NO vertex exists for a peer — the listing is synthesized on every read, so the module
    // still holds exactly its two CONNECTIONS however many peers are audible (and since S4
    // the `conn` creator endpoint is not among them).
    assert.deepEqual(listingNames(await cli.bus.readField(['net', 'ws-server'], ':children[]')),
                     ['ctrl', 'mesh']);
  });
});

/* ------------------------------------------------------------------ xfails --- */
/*
 * The absences this testbed EXPOSES rather than blocks on. See tests/testbed/README.md
 * for the full register.
 *
 * THESE DO NOT FORCE ANYTHING, and this comment used to claim they did (#606): "the day
 * the gap closes the assertion goes red and forces the update". A `todo` whose body is
 * entirely comments has no assertions, so it can never go red — the forcing function was
 * asserted, never built. It had already failed silently: the identity xfail sat here
 * describing a gap that #406 and #409 closed, while `compose.yml` in the same directory
 * gives every node a `--identity` and the #409 walk step depends on it working.
 *
 * They are kept as a documented register, honestly labelled. Promoting one to a real
 * assertion of current behaviour is the fix the README promises, and it needs the compose
 * stack — which is the `mesh-testbed` CI job, not a local run.
 */

test('xfail: killing a node does not drive its peers\' link state down (#407, #66)', { todo: true }, () => {
  // NARROWED (#606). The liveness half has LANDED: RFC-0014 §4 gave transport_vertex.hpp a
  // six-state link_state_t (:96) including RECONNECTING, plus backoff_ms (:134) and
  // connect_timeout_ms (:137). "There is no liveness signal, no reconnect anywhere" was
  // this test's claim and it is no longer true.
  //
  // What remains is the TEARDOWN half: no child removal, so a link cannot be recreated
  // under the same name after a failure (PATH_IN_USE) — recovery needs a NEW name, which is
  // why teardown is the hard blocker for the stable-identity reconnection #407 must design.
});

test('xfail: close_peer cannot evict a peer from a SPEC-created listener (#407)', { todo: true }, () => {
  // The documented eviction path is link_of(name)->bus()->close_peer(peer). Before #418 it
  // was dead for every builtin: link_of() resolves only config-constructed links, and a
  // config-constructed ws was never peer_named, so bus() was null. #418 makes the bus's
  // listener both config-constructed AND peer_named, so the path is now REACHABLE — but
  // there is still no in-band surface to invoke it: eviction needs the removal model #407
  // owns. Promote this to a real assertion when #407 lands.
});
