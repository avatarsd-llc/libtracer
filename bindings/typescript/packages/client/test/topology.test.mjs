// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief #409 / ADR-0044 pt 3 — the client-side topology projection, against the REAL
 * cyclic mesh (#408).
 *
 * The mesh testbed is the right proving ground precisely because it contains a
 * deliberate physical cycle (a→b→c→a). That cycle is what separates the two modes:
 *
 *   - **pre-identity** — there is no way to know `/b` and `/b/c/a/b` are one device, so
 *     the walk cannot terminate on its own and `maxDepth` is the only bound. This test
 *     asserts that degradation EXACTLY, rather than hiding it: the walk is bounded,
 *     `truncated` is true, `authoritative` is false, and node count grows with depth.
 *     That is the executable argument for #406.
 *   - **with identity** — nodes collapse, the cycle closes, and the walk terminates on
 *     its own. `identify()` reads the REAL RFC-0011 `:identity` facet (#406): each node
 *     is started with a distinct `--identity` key, and the walk dedups on those bytes.
 *     Note what it does NOT need: credentials. The facet is pre-auth (§C.2), which is
 *     precisely the position a setup edge is in when it meets a network for the first
 *     time.
 *
 * GUARDED on LIBTRACER_MESH_CTRL (the same env the mesh-testbed job sets); plain
 * `npm test` without it SKIPS.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
import { TransportWs } from '@avatarsd-llc/libtracer-ws';
import { LibtracerClient, walkTopology, routeKey } from '../dist/index.js';

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

const CTRL = parseEndpoints(process.env.LIBTRACER_MESH_CTRL);
const skip = !CTRL.a ? 'set LIBTRACER_MESH_CTRL (bring up tests/testbed/compose.yml)' : false;

const utf8 = new TextDecoder();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function connect(name, budgetMs = 60000) {
  const { host, port } = CTRL[name];
  const deadline = Date.now() + budgetMs;
  let lastErr;
  while (Date.now() < deadline) {
    const transport = new TransportWs(`ws://${host}:${port}`, { WebSocket });
    try {
      await transport.connect();
      return {
        client: new LibtracerClient(transport, { replyEndpoint: ['driver'], requestTimeoutMs: 20000 }),
        transport,
      };
    } catch (err) {
      lastErr = err;
      await transport.close().catch(() => {});
      await sleep(250);
    }
  }
  throw new Error(`ctrl connect to ${name} timed out: ${lastErr}`);
}

/**
 * @brief The REAL identity resolver: read the RFC-0011 `:identity` facet (#406).
 *
 * `SETTINGS{ NAME "kind" VALUE u8, NAME "key" VALUE <32 bytes> }`. The key bytes are the
 * cross-path dedup key ADR-0044 pt 3 left to the client — this is the whole point of the
 * facet. A keyless node returns `SCHEMA_NOT_FOUND`, which surfaces here as `null` and
 * leaves that node route-distinct and the graph non-authoritative.
 *
 * Note it is served PRE-AUTH (RFC-0011 §C.2), so a walker needs no credentials to build
 * the map — which is exactly what a setup edge meeting a network for the first time has.
 */
async function identifyByFacet(client, route) {
  let record;
  try {
    // ANY registered vertex answers — the record is the NODE's (RFC-0011 §C.1), which is
    // exactly why a walker that has just crossed a link can identify where it landed
    // without first discovering the peer's root. /node/name exists on every mesh node.
    record = await client.readField([...route, 'node', 'name'], ':identity');
  } catch {
    return null; // keyless node (SCHEMA_NOT_FOUND) or unreachable
  }
  // Positional members, in the fixed order §B pins: kind/<u8>, key/<bytes>.
  const kids = record.children ?? [];
  if (kids.length < 4) return null;
  const kind = kids[1].payload?.[0];
  const key = kids[3].payload;
  if (kind === undefined || !key) return null;
  return `k${kind}:${Buffer.from(key).toString('hex')}`;
}

/** @brief The driver's own ctrl link — never walk back out of the node we came in on. */
const skipCtrl = (name) => name === 'ctrl';

/** @brief The identity each compose/local node is started with — its --identity key. */
const A = 'k1:' + 'a1'.repeat(32);
const B = 'k1:' + 'b2'.repeat(32);
const C = 'k1:' + 'c3'.repeat(32);
const H = 'k1:' + '40'.repeat(32);

test('topology: WITHOUT identity the cyclic mesh cannot terminate — maxDepth is the only bound', { skip }, async (t) => {
  const { client, transport } = await connect('a');
  t.after(() => transport.close().catch(() => {}));

  const shallow = await walkTopology(client, { maxDepth: 3, skipLink: skipCtrl });
  const deep = await walkTopology(client, { maxDepth: 6, skipLink: skipCtrl });

  assert.equal(shallow.authoritative, false, 'no identity => the graph is a shape, not a map');
  assert.equal(shallow.truncated, true, 'the ring cannot close, so the walk truncates');
  assert.ok(
    shallow.warnings.some((w) => w.includes('#406')),
    'the result names WHY it could not terminate',
  );
  for (const n of shallow.nodes) {
    assert.equal(n.identity, null);
    assert.equal(n.routes.length, 1, 'pre-identity every node is exactly one route — nothing collapses');
  }

  // The load-bearing assertion: deepening the walk finds MORE nodes, because the same
  // three devices keep reappearing under longer routes. A terminating walk would return
  // the same node set at any sufficient depth.
  assert.ok(
    deep.nodes.length > shallow.nodes.length,
    `node count must grow with depth while devices repeat (3 hops: ${shallow.nodes.length}, 6 hops: ${deep.nodes.length})`,
  );

  // And concretely: node `a` — the vantage — is reached again as /b/a and is NOT
  // recognized as itself. ADR-0044 pt 2, made visible.
  const ids = deep.nodes.map((n) => n.id);
  assert.ok(ids.includes('/'), 'the vantage is the root node');
  assert.ok(ids.includes('/b/a'), 'the vantage is ALSO reached as /b/a — as a separate node');
  assert.ok(ids.includes('/b/a/b/a'), 'and again as /b/a/b/a — the same device, four nodes deep');
  t.diagnostic(`pre-identity: ${deep.nodes.length} nodes for 4 real devices — the cycle is unrolled, not closed`);
});

test('topology: WITH identity the cycle closes, nodes collapse, and the walk self-terminates', { skip }, async (t) => {
  const { client, transport } = await connect('a');
  t.after(() => transport.close().catch(() => {}));

  // maxDepth well past the ring's circumference: a terminating walk stops on its own.
  const g = await walkTopology(client, {
    maxDepth: 12,
    identify: identifyByFacet,
    skipLink: skipCtrl,
  });

  assert.equal(g.authoritative, true, 'every node identified => the graph is a real map');
  assert.equal(g.truncated, false, 'identity closed the ring, so the walk ended on its own');

  // Exactly the four devices the compose stack runs — no matter how many routes reach them.
  const ids = g.nodes.map((n) => n.id).sort();
  assert.deepEqual(ids, [A, B, C, H].sort());
  assert.equal(g.root, A, 'the vantage is node a — identified by its KEY, not a name');

  // The collapse itself: the vantage is reached again from BOTH its ring neighbours,
  // and the walk PROVED all three routes are one device. Pre-identity these were three
  // unrelated nodes (see the previous test).
  const a = g.nodes.find((n) => n.id === A);
  assert.deepEqual(a.routes.map(routeKey).sort(), ['/', '/b/a', '/c/a']);

  // Every link is seen from BOTH ends, because a link is bidirectional even though the
  // DIAL that created it was not: `a` dialing `b` creates link "b" at a AND link "a" at
  // b, and either end routes through its own name. So the 5 dials surface as 8 walkable
  // edges — the hub's two inbound links are the exception: they land on its single
  // peer_named `mesh` bus, which is not descendable (next test).
  const wire = g.edges.map((e) => `${e.from} -${e.name}-> ${e.to}`).sort();
  assert.deepEqual(wire, [
    `${A} -b-> ${B}`,
    `${A} -c-> ${C}`,
    `${A} -hub-> ${H}`,
    `${B} -a-> ${A}`,
    `${B} -c-> ${C}`,
    `${B} -hub-> ${H}`,
    `${C} -a-> ${A}`,
    `${C} -b-> ${B}`,
  ].sort());
  t.diagnostic('with identity: 4 nodes, 8 edges, ring closed, walk self-terminated — the ADR-0044 pt-3 projection');
});

test('topology: hub is reached by two independent routes and collapses to one node', { skip }, async (t) => {
  const { client, transport } = await connect('a');
  t.after(() => transport.close().catch(() => {}));

  const g = await walkTopology(client, { maxDepth: 12, identify: identifyByFacet, skipLink: skipCtrl });
  const hub = g.nodes.find((n) => n.id === H);
  const routes = hub.routes.map(routeKey).sort();

  // a->hub directly, and a->b->hub. Two paths, one device: the dedup ADR-0044 says the
  // core will never do and the client must.
  assert.deepEqual(routes, ['/b/hub', '/hub']);
  assert.equal(g.edges.filter((e) => e.to === H).length, 2, 'two distinct edges reach the one hub');
});

test('topology: a BUS link is reported with its peers, never descended', { skip }, async (t) => {
  const { client, transport } = await connect('a');
  t.after(() => transport.close().catch(() => {}));

  const g = await walkTopology(client, { maxDepth: 12, identify: identifyByFacet, skipLink: skipCtrl });

  // The hub's `mesh` listener is peer_named: ONE connection serving both a and b.
  const bus = g.busLinks.find((b) => b.name === 'mesh');
  assert.ok(bus, 'the peer_named listener is reported as a bus link');
  assert.equal(bus.at, H);
  assert.equal(bus.peers.length, 2, 'it hears exactly its two dialers');
  for (const p of bus.peers) assert.match(p, /^\d+\.\d+\.\d+\.\d+:\d+$/);

  // The finding this pins: a ws bus names peers <ip>:<port>, and both "." and ":" are
  // reserved in a path segment (reference/03) — so ADR-0044's "each listed name doubles
  // as a routable next-hop segment" is TRUE for a CAN bus (n5/n7) and FALSE here. The
  // peers are enumerable but not addressable, from any conforming client.
  assert.equal(bus.routable, false, 'ws peer names are not legal path segments');
  assert.ok(
    g.warnings.some((w) => w.includes('mesh') && w.includes('not legal path segments')),
    'the walk says WHY it stopped, rather than silently truncating',
  );

  // And it must not have been walked as a node — routing through a bus link's NAME
  // broadcasts to every peer, drawing N replies for one request.
  assert.equal(g.edges.some((e) => e.name === 'mesh'), false, 'no edge descends the bus link');
  t.diagnostic(`bus link hub/mesh: peers=${JSON.stringify(bus.peers)} routable=${bus.routable}`);
});
