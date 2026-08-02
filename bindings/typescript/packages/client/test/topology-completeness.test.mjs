// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief #676 — a `walkTopology` result must be able to say it is INCOMPLETE.
 *
 * `authoritative` keys on identity resolution alone, so it stays `true` on a graph that
 * is missing whole subtrees — and the doc comment told callers to check exactly that
 * flag. The fix splits the boolean: `complete` is cleared by every site that drops data,
 * and each loss is itemized in `gaps`.
 *
 * There are exactly THREE such sites, and this file has one test per site that fails if
 * that site stops clearing the flag:
 *
 *   1. the `/net` module listing  — the node becomes a leaf, its whole subtree vanishes
 *   2. one module's `:children[]` — that module's links vanish
 *   3. a link's peer listing      — the link's shape is guessed, a bus is under-reported
 *
 * Plus the discrimination the blanket catch used to lose: `NOT_FOUND` is an ANSWER (a
 * genuine leaf), not a loss, and must cost nothing.
 *
 * These run against a stub transport — no testbed, no env guard — because the point is
 * the failure paths, which a healthy mesh does not exercise.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { TYPE } from '@avatarsd-llc/libtracer';
import { walkTopology, routeKey, FwdError, FWD_ERROR } from '../dist/index.js';

/**
 * @brief An identity resolver that always answers, so `authoritative` is TRUE.
 *
 * That is the whole point of these tests: with identity resolved the old single flag
 * says the graph is trustworthy, no matter how much of the network went missing. Keying
 * on the route keeps node ids identical to the pre-identity form.
 */
const identifyByRoute = async (_client, route) => routeKey(route);

const utf8 = new TextEncoder();

/** @brief A `:children[]` POINT listing carrying `names` — the shape `listingNames` reads. */
function listing(names) {
  return {
    type: TYPE.POINT,
    children: names.map((n) => ({
      type: TYPE.POINT,
      children: [{ type: TYPE.NAME, payload: utf8.encode(n) }],
    })),
  };
}

/**
 * @brief A stub client whose only surface `walkTopology` touches is `readField`.
 *
 * `routes` maps `"<path> <field>"` to a listing, a thrown error, or a function. Anything
 * unlisted answers `NOT_FOUND` — the "nothing there" reply, so an unmentioned vertex is a
 * legitimate leaf rather than an accidental gap.
 */
function stubClient(routes) {
  const calls = [];
  const client = {
    async readField(route, field) {
      const key = '/' + route.join('/') + ' ' + field;
      calls.push(key);
      const answer = routes[key];
      if (answer === undefined) throw new FwdError(FWD_ERROR.NOT_FOUND);
      if (typeof answer === 'function') return answer();
      if (answer instanceof Error) throw answer;
      return answer;
    },
  };
  return { client, calls };
}

/** @brief A two-node network: root -> `b` over module `ws-client`. */
const HEALTHY = {
  '/net :children[]': listing(['ws-client']),
  '/net/ws-client :children[]': listing(['b']),
  '/net/ws-client/b :children[]': listing([]),
  '/net/ws-client/b/net :children[]': listing(['ws-server']),
  '/net/ws-client/b/net/ws-server :children[]': listing(['a']),
};

test('#676 baseline: a walk that reads everything is complete, with no gaps', async () => {
  const { client } = stubClient(HEALTHY);
  const g = await walkTopology(client, { identify: identifyByRoute, maxDepth: 2 });

  assert.equal(g.complete, true, 'nothing was dropped');
  assert.deepEqual(g.gaps, []);
  assert.deepEqual(g.pruned, []);
  // root, b, and the link back from b — which is route-distinct here because this
  // resolver keys on the route, exactly as the pre-identity walk does.
  assert.equal(g.nodes.length, 3);
});

test('#676 site 1: a failed /net read makes a leaf and clears complete', async () => {
  // b refuses its /net read. Under the old code b silently became a leaf, its subtree
  // vanished, and the result still said authoritative:true.
  const { client } = stubClient({
    ...HEALTHY,
    '/net/ws-client/b/net :children[]': new FwdError(FWD_ERROR.PERMISSION_DENIED),
  });
  const g = await walkTopology(client, { identify: identifyByRoute, maxDepth: 4 });

  assert.equal(g.complete, false, 'a lost subtree is an incomplete graph');
  assert.equal(g.gaps.length, 1);
  assert.equal(g.gaps[0].site, 'net', 'the gap names the site that dropped the subtree');
  assert.deepEqual(g.gaps[0].route, ['net', 'ws-client', 'b']);
  assert.equal(g.gaps[0].at, '/net/ws-client/b');
  assert.equal(g.gaps[0].code, FWD_ERROR.PERMISSION_DENIED);
  assert.equal(g.gaps[0].codeName, 'PERMISSION_DENIED');
  assert.equal(g.gaps[0].module, null);

  // The regression itself: the flag the docs told callers to check is NOT the one that
  // moves here. If `complete` did not exist, this graph would look trustworthy.
  assert.equal(g.authoritative, true, 'identity resolution was never in question');
  assert.equal(g.truncated, false, 'nor did the walk hit maxDepth');
});

test('#676 site 2: a failed module read clears complete and names the module', async () => {
  const { client } = stubClient({
    ...HEALTHY,
    '/net :children[]': listing(['ws-client', 'can']),
    '/net/can :children[]': new FwdError(FWD_ERROR.PERMISSION_DENIED),
  });
  const g = await walkTopology(client, { identify: identifyByRoute, maxDepth: 1 });

  assert.equal(g.complete, false, "a module's links are missing");
  assert.equal(g.gaps.length, 1);
  assert.equal(g.gaps[0].site, 'module');
  assert.equal(g.gaps[0].module, 'can');
  assert.equal(g.gaps[0].at, '/');
  assert.equal(g.gaps[0].code, FWD_ERROR.PERMISSION_DENIED);
  assert.equal(g.authoritative, true, 'again, identity is unaffected');
});

test('#676 site 3: a failed peer read clears complete — the link shape was guessed', async () => {
  // peersOf swallows the failure and assumes point-to-point. That guess may descend a
  // BUS link by name, which broadcasts — so the caller must be told it was a guess.
  const { client } = stubClient({
    ...HEALTHY,
    '/net/ws-client/b :children[]': new FwdError(FWD_ERROR.PERMISSION_DENIED),
  });
  const g = await walkTopology(client, { identify: identifyByRoute, maxDepth: 2 });

  assert.equal(g.complete, false, 'the link shape is unknown, so the graph is not complete');
  assert.equal(g.gaps.length, 1);
  assert.equal(g.gaps[0].site, 'peers');
  assert.equal(g.gaps[0].module, 'ws-client');
  assert.equal(g.gaps[0].name, 'b');
  assert.equal(g.authoritative, true);
});

test('#676 NOT_FOUND is an answer, not a loss: a genuine leaf costs no completeness', async () => {
  // b has no /net at all — the stub answers NOT_FOUND for every unlisted vertex, which
  // is precisely a node with no transport vertices.
  const { client } = stubClient({
    '/net :children[]': listing(['ws-client']),
    '/net/ws-client :children[]': listing(['b']),
  });
  const g = await walkTopology(client, { identify: identifyByRoute, maxDepth: 4 });

  assert.equal(g.complete, true, 'a leaf that says "nothing here" hid nothing');
  assert.deepEqual(g.gaps, []);
  assert.deepEqual(g.warnings, [], 'and it produces no warning either');
  assert.equal(g.nodes.length, 2, 'b is still a node — it is just a leaf');
});

test('#676 a dead node is not re-paid per module: a transport-class failure prunes it', async () => {
  // Three modules on b. A node that has stopped answering used to cost one
  // requestTimeoutMs PER module; the walk must stop asking after the first.
  const { client, calls } = stubClient({
    ...HEALTHY,
    '/net/ws-client/b/net :children[]': listing(['m1', 'm2', 'm3']),
    '/net/ws-client/b/net/m1 :children[]': new Error('request timed out after 10000ms (no FWD reply)'),
    '/net/ws-client/b/net/m2 :children[]': listing(['x']),
    '/net/ws-client/b/net/m3 :children[]': listing(['y']),
  });
  const g = await walkTopology(client, { identify: identifyByRoute, maxDepth: 2 });

  assert.equal(
    calls.filter((c) => c.startsWith('/net/ws-client/b/net/m')).length,
    1,
    'only the first module is attempted — m2 and m3 are not re-paid',
  );
  assert.deepEqual(g.pruned, [['net', 'ws-client', 'b']], 'the abandoned route is reported');
  assert.equal(g.complete, false);
  assert.equal(g.gaps[0].code, null, 'no reply means no wire ERROR code — that is the tell');
});
