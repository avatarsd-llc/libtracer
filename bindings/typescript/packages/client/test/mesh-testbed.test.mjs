// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief #408 — the decentralized MESH testbed driver: form an arbitrary,
 * cycle-containing multi-node topology the REAL way, then route across it.
 *
 * The driver is a third libtracer node (the TS client SDK) wearing the ORCHESTRATING
 * hat (reference/13): it holds a ctrl link to each device, DIALS every inter-node link
 * by writing a `SPEC` into that device's `/net:children[]` **remotely**, and then routes
 * through the mesh it just made. That is the web-ui-as-setup-edge story executed
 * end to end — no `provide_link`, no config file, no test seam: every link is a real ws
 * socket the built-in `ws` factory constructs from a SPEC's config.
 *
 * TOPOLOGY — a 3-node ring (the deliberate physical CYCLE) plus a peer-enumeration hub:
 *
 *      a ──dial──▶ b ──dial──▶ c ──dial──▶ a        (the ring closes: a physical cycle)
 *      a ──dial──▶ hub ◀──dial── b                  (two peers on ONE peer_named listener)
 *
 * NAMING RULE: every node names every link after the node at the FAR end. That is what
 * makes replies retrace: each forwarder prepends its own name for the ARRIVAL link to
 * `src`, and the terminus answers over the arrival link (RFC-0004 §B).
 *
 * ADDRESSING: a connection's routing key is its BARE name — `/b/c/node/name`, NOT
 * `/net/b/net/c/node/name` as reference/03 + /07 currently claim (see #419). These
 * assertions PIN the implementation; they are the repo's first two-forwarder coverage
 * (every existing FWD test has exactly one).
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
import { LibtracerClient, encodeValue, encodeConnSpec } from '../dist/index.js';

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

const NODES = ['a', 'b', 'c', 'hub'];
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

/**
 * @brief Remotely DIAL a link: write a client SPEC into `node`'s `/net:children[]`.
 *
 * Retries the WHOLE write: a built-in DIAL is synchronous with no retry, and a refused
 * dial returns before `register_vertex_key` — leaving NO vertex — so re-issuing the same
 * SPEC is the correct (and only) recovery. `linkName` is the FAR node's name, per the
 * naming rule.
 */
async function dial(client, from, linkName, toNode, budgetMs = 30000) {
  const { host, port } = PEERS[toNode];
  const spec = encodeConnSpec({
    type: 'client',
    name: linkName,
    role: 'dial',
    port,
    kind: 'ws',
    addr: host,
  });
  const deadline = Date.now() + budgetMs;
  let lastErr;
  while (Date.now() < deadline) {
    try {
      await client.writeField(['net'], ':children[]', spec);
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
  // The hub: two peers on ONE peer_named listener (ADR-0044 Brick C).
  await dial(cli.a, 'a', 'hub', 'hub');
  await dial(cli.b, 'b', 'hub', 'hub');
  t.diagnostic('mesh formed: ring a->b->c->a plus a,b->hub — every link an in-band SPEC');

  await t.test('each node reports the connections it was told to create', async () => {
    // Ordinary vertex enumeration of /net (NOT peer enumeration).
    assert.deepEqual(listingNames(await cli.a.readField(['net'], ':children[]')), ['b', 'c', 'ctrl', 'hub']);
    assert.deepEqual(listingNames(await cli.b.readField(['net'], ':children[]')), ['a', 'c', 'ctrl', 'hub']);
    assert.deepEqual(listingNames(await cli.c.readField(['net'], ':children[]')), ['a', 'b', 'ctrl']);
    assert.deepEqual(listingNames(await cli.hub.readField(['net'], ':children[]')), ['ctrl', 'mesh']);
  });

  await t.test('a local read resolves at the terminus (no hop)', async () => {
    assert.equal(await nameVia(cli.a, []), 'a');
    assert.equal(await nameVia(cli.c, []), 'c');
  });

  await t.test('ONE hop: /b/node/name reaches b', async () => {
    assert.equal(await nameVia(cli.a, ['b']), 'b');
  });

  await t.test('TWO hops: /b/c/node/name reaches c through b', async () => {
    // The repo's first two-forwarder assertion. a strips "b" -> forwards to b; b strips
    // "c" -> forwards to c; c resolves /node/name locally and the REPLY retraces
    // c -> b -> a -> driver by the accumulated src.
    assert.equal(await nameVia(cli.a, ['b', 'c']), 'c');
  });

  await t.test('the /net-prefixed address the docs promise does NOT resolve (#419)', async () => {
    // reference/03:206 + /07:150 claim /net/b/net/c/... — it is stale. "net" misses the
    // child-link registry, falls through to the local terminus, and graph.find gets the
    // whole key. Pinned so the docs cannot quietly become true without this going red.
    await assert.rejects(() => nameVia(cli.a, ['net', 'b', 'net', 'c']));
  });

  await t.test('CYCLE: an orbiting dst is not rejected — it terminates by dst exhaustion', async () => {
    // a -> b -> c -> a: a full orbit of the physical cycle, back to the origin node.
    assert.equal(await nameVia(cli.a, ['b', 'c', 'a']), 'a');
    // Two full orbits. There is NO visited-set and NO hop counter anywhere: loop-freedom
    // holds ONLY because dst is consumed monotonically, so a route is as long as it says
    // it is. The ERROR{tr::path::invalid} that reference/03:208 promises on a revisit does
    // not exist and cannot (the forwarder is stateless by design) — see #420.
    assert.equal(await nameVia(cli.a, ['b', 'c', 'a', 'b', 'c', 'a']), 'a');
    // The corollary that matters: a RECURSIVE WALK gets no protection from any of this.
    // Terminating one needs client-side identity-keyed dedup (#406 -> #409).
  });

  await t.test('a remote read through 2 hops returns the byte-exact seeded VALUE', async () => {
    const tlv = await cli.a.read(['b', 'c', 'sensor', 'temp']);
    assert.deepEqual(new Uint8Array(tlv.payload), le32(SEEDED_TEMP));
  });

  await t.test('a remote WRITE through 2 hops lands, and reads back', async () => {
    await cli.a.write(['b', 'c', 'sensor', 'temp'], encodeValue(le32(PUSHED_SAMPLE)));
    const tlv = await cli.a.read(['b', 'c', 'sensor', 'temp']);
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
    await cli.a.subscribe(['b', 'c', 'sensor', 'temp'], (value) => {
      seen.push(new Uint8Array(value));
      resolveFirst();
    });
    // /sensor/temp is transient-local (durability=1), so the subscribe LATCHES the
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

  await t.test('ADR-0044 Brick C: the hub lists its live peers from real traffic', async () => {
    // /net/mesh is a peer_named ws listener CREATED IN BAND (its peer_named key is
    // ws-private config, parsed by the ws factory — ADR-0043 §5). Both a and b dialled it.
    const listing = await cli.hub.readField(['net', 'mesh'], ':children[]');
    const peers = listingNames(listing);
    assert.equal(peers.length, 2, `the hub hears exactly its 2 dialers (got ${JSON.stringify(peers)})`);
    // Peer names are the far side's <ip>:<port>; the source port is ephemeral, so assert
    // the shape and the source addresses, never a literal.
    for (const p of peers) assert.match(p, /^\d+\.\d+\.\d+\.\d+:\d+$/);
    const ips = peers.map((p) => p.slice(0, p.lastIndexOf(':'))).sort();
    assert.deepEqual(ips, [PEERS.a.host, PEERS.b.host].sort(), 'the peers are exactly a and b');

    // NO vertex exists for a peer — the listing is synthesized on every read.
    assert.deepEqual(listingNames(await cli.hub.readField(['net'], ':children[]')), ['ctrl', 'mesh']);
  });
});

/* ------------------------------------------------------------------ xfails --- */
/*
 * The absences this testbed EXPOSES rather than blocks on. Each is a `todo` that
 * documents current behaviour, so the day the gap closes the assertion goes red and
 * forces the update. See tests/testbed/README.md for the full register.
 */

test('xfail: a node has no identity independent of the path it was reached by (#406, RFC-0011)', { todo: true }, () => {
  // /node/name is a SEEDED APPLICATION VALUE, not an identity — a node can claim anything,
  // and nothing binds the claim to the device. There is no identity surface at all today:
  // peer_id_t has zero call sites, and RFC-0011's `:identity` facet is unmerged.
  //
  // The consequence is exactly what the ring above demonstrates: `b` reached as /b and as
  // /c/a/b is TWO UNRELATED PATHS, and libtracer will never tell you they are one device
  // (ADR-0044 pt 2 — the core never dedups, at any layer, on any node). So a recursive
  // topology walk cannot terminate on the cycle by itself: dedup is the client's job, keyed
  // by an identity it chooses (#409's stitch utility), and #406 is the keystone that makes
  // that key trustworthy rather than a guess.
});

test('xfail: killing a node does not drive its peers\' link state down (#407, #66)', { todo: true }, () => {
  // set_link_state(name, true) is called exactly once, at creation. Kill container `c` and
  // /net/c on b still reads UP forever: there is no liveness signal, no reconnect anywhere
  // ("Reconnect is out of scope" — transport_tcp.hpp), and no child removal, so a link
  // cannot even be recreated under the same name after a failure (PATH_IN_USE). Recovery
  // needs a NEW name — which is precisely why teardown is a hard blocker for the
  // stable-identity reconnection #407 has to design.
});

test('xfail: close_peer cannot evict a peer from a SPEC-created listener (#407)', { todo: true }, () => {
  // The documented eviction path is link_of(name)->bus()->close_peer(peer). Before #418 it
  // was dead for every builtin: link_of() resolves only config-constructed links, and a
  // config-constructed ws was never peer_named, so bus() was null. #418 makes the hub's
  // listener both config-constructed AND peer_named, so the path is now REACHABLE — but
  // there is still no in-band surface to invoke it: eviction needs the removal model #407
  // owns. Promote this to a real assertion when #407 lands.
});
