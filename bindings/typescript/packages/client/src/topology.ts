// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Topology walk — the client-side projection of the decentralized graph
 * (#409, ADR-0044 pt 3).
 *
 * A libtracer node's graph is rooted **at the node you are asking**. There is no
 * global root: the whole-network view is a **projection assembled by a client**, by
 * descending through transport vertices and composing routes
 * ([reference/03](../../../../docs/reference/03-addressing.md): *"global scope is a
 * logical view assembled by composing routes"*). ADR-0044 is emphatic that this is
 * the client's job and never the core's:
 *
 * > separate paths stay separate; libtracer never matches device identities across
 * > paths, at any layer, on any node. The deduplicated "real graph" is client/app
 * > logic, keyed by an identity **it** chooses.
 *
 * This module is that logic. It walks `:children[]` under each node's `/net`, treats
 * every connection NAME as a routable next hop, and stitches what it finds into a
 * node/edge model a renderer can draw.
 *
 * ## Addressing (the part that surprises people)
 *
 * A connection's **routing key is its bare NAME**, not its `/net/<name>` vertex key.
 * `/net/b` is where you *read* the link's state; `b` is the first `dst` segment that
 * *routes through* it. So the walk composes routes as `[] → ['b'] → ['b','c']`, and
 * reads each node's connections at `[...route, 'net']:children[]`. (reference/03 and
 * /07 currently document a `/net/`-prefixed form that does not resolve — see #419.)
 *
 * ## Termination — read this before trusting a result
 *
 * **Without a node identity, this walk cannot terminate on its own.** A physical
 * cycle (a→b→c→a) yields infinitely many *distinct* routes — `/b`, `/b/c`, `/b/c/a`,
 * `/b/c/a/b`, … — and libtracer will never tell you that `/b` and `/c/a/b` are the
 * same device. The router does not help either: the `ERROR{tr::path::invalid}` on a
 * revisit that reference/03 promises **does not exist** (#420); loop-freedom holds
 * only because a `dst` is consumed monotonically, which protects a *delivery*, not a
 * *walk*.
 *
 * So termination has exactly two sources, and only one of them is sound:
 *
 * - **`identify` supplied** → nodes collapse by identity, the cycle closes, and the
 *   result is marked {@link TopologyGraph.authoritative}. This needs the node
 *   identity facet (#406 / RFC-0011), which does not exist yet.
 * - **no `identify`** → the walk is bounded by {@link WalkOptions.maxDepth} and
 *   nothing else. Each route becomes its own node, one device appears many times,
 *   and the result is **non-authoritative**: a shape, not a map.
 *
 * The degraded mode is deliberately kept honest rather than made to look right by a
 * heuristic. Guessing that two routes are one device — by matching a seeded name, a
 * peer address, a value — would be exactly the identity-matching ADR-0044 forbids the
 * core from doing, moved into the client and dressed up. When the answer is unknown,
 * this says so.
 */

import type { LibtracerClient } from './client.js';
import type { Tlv } from '@avatarsd-llc/libtracer';
import { TYPE } from '@avatarsd-llc/libtracer';

const utf8 = new TextDecoder();

/**
 * @brief Characters reference/03 forbids inside a path segment (mirrors `tlv.ts`).
 *
 * Used to decide whether a bus peer's name could even be addressed as a hop — a ws
 * bus names peers `<ip>:<port>`, which contains two of these.
 */
const RESERVED_SEGMENT_CHARS = /[/:.[\]*?]/;

/** @brief One node of the stitched graph — a device, or (pre-identity) one route to one. */
export interface TopologyNode {
  /** @brief Stable within a single walk: the identity when known, else the route. */
  readonly id: string;
  /**
   * @brief Every route by which this node was reached, from the walk's vantage.
   *
   * More than one route means the walk **proved** they are the same device — which
   * only ever happens when {@link WalkOptions.identify} is supplied. `[]` is the
   * vantage node itself.
   */
  readonly routes: string[][];
  /** @brief The node's identity, or `null` when the walk could not read one. */
  readonly identity: string | null;
}

/** @brief One connection, as seen from the node that owns it. */
export interface TopologyEdge {
  /** @brief The {@link TopologyNode.id} owning the connection. */
  readonly from: string;
  /** @brief The {@link TopologyNode.id} the connection reaches. */
  readonly to: string;
  /** @brief The connection NAME at `from` — also the `dst` segment that routes here. */
  readonly name: string;
}

/**
 * @brief A connection the walk found but could not descend: a BUS link with live peers.
 *
 * A bus link (ADR-0044 — a transport serving many peers, e.g. a `peer_named` ws
 * listener or a CAN segment) is a **dead end for this walk**, for a reason worth
 * stating precisely because it looks like a bug otherwise:
 *
 * - Routing a `dst` through the **connection NAME** reaches the transport's `send()`,
 *   which **broadcasts to every peer**. One request then draws N replies, which
 *   corrupts a client's reply correlation — so descending by name is not merely
 *   imprecise, it is actively wrong.
 * - The directed hop is the **enumerated peer name** (the registry's peer fallback).
 *   That works for a CAN bus, whose peers are named `n5`/`n7` — but a ws bus names its
 *   peers `<ip>:<port>`, and both `:` and `.` are **reserved characters that may not
 *   appear in a path segment** (reference/03). So a ws bus's peers are enumerable but
 *   **not addressable** by the path grammar, from any conforming client.
 *
 * The peers are therefore reported here — the walk knows they exist and can name them
 * — but they are not nodes, because nothing behind them can be read.
 */
export interface TopologyBusPeers {
  /** @brief The {@link TopologyNode.id} owning the bus connection. */
  readonly at: string;
  /** @brief The connection NAME of the bus link. */
  readonly name: string;
  /** @brief The peers it currently hears, as the transport names them. */
  readonly peers: string[];
  /** @brief True when every peer name is a legal path segment, so a directed hop is expressible. */
  readonly routable: boolean;
}

/** @brief The assembled projection. */
export interface TopologyGraph {
  readonly nodes: TopologyNode[];
  readonly edges: TopologyEdge[];
  /** @brief Bus links found but not descended, with the peers they hear (see {@link TopologyBusPeers}). */
  readonly busLinks: TopologyBusPeers[];
  /** @brief The {@link TopologyNode.id} of the vantage — the node the client is attached to. */
  readonly root: string;
  /** @brief The walk hit {@link WalkOptions.maxDepth} and stopped early: the graph is incomplete. */
  readonly truncated: boolean;
  /**
   * @brief True only if EVERY node reported an identity, so nodes are really devices.
   *
   * When false the graph is a **shape, not a map**: one device may appear as several
   * nodes, and a cycle appears as an unbounded chain truncated by `maxDepth`. Do not
   * present a non-authoritative graph as the network's topology.
   */
  readonly authoritative: boolean;
  /** @brief Non-fatal problems (an unreadable node, a failed identity read). */
  readonly warnings: string[];
}

/** @brief How a walk resolves node identity and how far it goes. */
export interface WalkOptions {
  /**
   * @brief Hop limit. Default 8.
   *
   * WITHOUT {@link WalkOptions.identify} this is the ONLY thing that stops the walk on
   * a cyclic topology — keep it modest. A route may not exceed the protocol's 32-segment
   * PATH cap in any case, and every hop costs a round trip across the whole route.
   */
  readonly maxDepth?: number;
  /** @brief The transport-vertex parent segment. Default `"net"`. */
  readonly netRoot?: string;
  /**
   * @brief Read the identity of the node at `route`, or `null` if it has none.
   *
   * The dedup key ADR-0044 pt 3 leaves to the client. Supplying it is what makes a
   * walk terminate on a cycle and what makes the result authoritative. The intended
   * implementation reads the `:identity` facet (#406 / RFC-0011) and returns the
   * public key; a keyless node returns `null` and stays route-distinct.
   *
   * A `null` for SOME nodes is handled: those stay route-distinct and the graph is
   * reported non-authoritative.
   */
  readonly identify?: (client: LibtracerClient, route: string[]) => Promise<string | null>;
  /**
   * @brief Return true to NOT descend through connection `name` at `route`.
   *
   * The walk cannot tell an inter-node link from the link it arrived on — every
   * connection is just a NAME — so a caller that knows its own edge should skip it
   * (e.g. a setup-edge web UI skipping the `ctrl` link back to itself).
   */
  readonly skipLink?: (name: string, route: string[]) => boolean;
}

/** @brief `["b","c"]` → `"/b/c"`; `[]` → `"/"` (the vantage). */
export function routeKey(route: string[]): string {
  return route.length === 0 ? '/' : '/' + route.join('/');
}

/** @brief The NAME strings of a `:children[]` POINT listing. */
function listingNames(tlv: Tlv): string[] {
  const names: string[] = [];
  for (const member of tlv.children ?? []) {
    for (const field of member.children ?? []) {
      if (field.type === TYPE.NAME) names.push(utf8.decode(field.payload));
    }
  }
  return names;
}

/**
 * @brief Walk the reachable topology from `client`'s vantage and stitch it into a graph.
 *
 * Breadth-first, so a truncated walk is a complete picture of the near network rather
 * than one deep tendril. Each node costs one `readField` for its connections plus one
 * {@link WalkOptions.identify} call, both routed across the full path to it.
 *
 * An unreadable node (a dead link, a denied read) becomes a warning and a leaf — the
 * walk continues, because one unreachable branch is not a reason to lose the rest.
 *
 * @param client attached to the vantage node
 * @param opts   identity resolution and bounds
 * @returns the projection — check {@link TopologyGraph.authoritative} before trusting it
 */
export async function walkTopology(
  client: LibtracerClient,
  opts: WalkOptions = {},
): Promise<TopologyGraph> {
  const maxDepth = opts.maxDepth ?? 8;
  const netRoot = opts.netRoot ?? 'net';

  const byId = new Map<string, { id: string; routes: string[][]; identity: string | null }>();
  const edges: TopologyEdge[] = [];
  const busLinks: TopologyBusPeers[] = [];
  const warnings: string[] = [];
  let truncated = false;
  let anyUnidentified = false;

  /**
   * A connection's `:children[]` discriminates the two link shapes (ADR-0044): a BUS
   * link synthesizes its live peers there, while a point-to-point link has no such
   * facet and the generic member listing of a childless vertex is empty. Empty ⇒ safe
   * to route through by name; non-empty ⇒ a bus, and routing by name would broadcast.
   */
  const peersOf = async (route: string[], name: string): Promise<string[]> => {
    try {
      return listingNames(await client.readField([...route, netRoot, name], ':children[]'));
    } catch {
      return []; // unreadable ⇒ treat as point-to-point; the descend below will warn if it fails
    }
  };

  /** Resolve `route` to a node id, creating the node on first sight. */
  const intern = async (route: string[]): Promise<{ id: string; fresh: boolean }> => {
    let identity: string | null = null;
    if (opts.identify) {
      try {
        identity = await opts.identify(client, route);
      } catch (err) {
        warnings.push(`identity read failed at ${routeKey(route)}: ${String(err)}`);
      }
    }
    if (identity === null) anyUnidentified = true;

    // The dedup key. With an identity, two routes to one device collapse — and a cycle
    // closes. Without one, the ROUTE is the key, so nothing ever collapses: a cycle
    // yields endless distinct nodes and only maxDepth stops the walk. That is the
    // degradation, stated in code.
    const id = identity ?? routeKey(route);
    const existing = byId.get(id);
    if (existing) {
      if (!existing.routes.some((r) => routeKey(r) === routeKey(route))) existing.routes.push(route);
      return { id, fresh: false };
    }
    byId.set(id, { id, routes: [route], identity });
    return { id, fresh: true };
  };

  const rootEntry = await intern([]);
  const root = rootEntry.id;

  let frontier: { route: string[]; id: string }[] = [{ route: [], id: root }];

  for (let depth = 0; depth < maxDepth && frontier.length > 0; depth++) {
    const next: { route: string[]; id: string }[] = [];

    for (const { route, id } of frontier) {
      let names: string[];
      try {
        const listing = await client.readField([...route, netRoot], ':children[]');
        names = listingNames(listing);
      } catch (err) {
        // A node with no /net has no connections — indistinguishable here from one that
        // refused the read. Either way it is a leaf, and either way the walk goes on.
        warnings.push(`could not read connections at ${routeKey(route)}: ${String(err)}`);
        continue;
      }

      for (const name of names) {
        if (opts.skipLink?.(name, route)) continue;

        // A bus link is a dead end: routing through its NAME broadcasts to every peer,
        // drawing N replies for one request. Record its peers and stop. (See
        // TopologyBusPeers for why the directed per-peer hop is not expressible either.)
        const peers = await peersOf(route, name);
        if (peers.length > 0) {
          const routable = peers.every((p) => !RESERVED_SEGMENT_CHARS.test(p));
          busLinks.push({ at: id, name, peers, routable });
          warnings.push(
            `${routeKey(route)} link "${name}" is a bus with ${peers.length} peer(s); not descended` +
              (routable ? '' : ' — its peer names are not legal path segments, so no directed hop exists'),
          );
          continue;
        }

        const childRoute = [...route, name];
        const child = await intern(childRoute);
        edges.push({ from: id, to: child.id, name });
        // Only descend into a node we have not already expanded — the dedup that makes
        // a cycle terminate. Pre-identity `fresh` is always true for a cyclic route, so
        // this never fires and maxDepth is the sole bound.
        if (child.fresh) next.push({ route: childRoute, id: child.id });
      }
    }

    frontier = next;
    if (frontier.length > 0 && depth + 1 >= maxDepth) truncated = true;
  }

  if (truncated) {
    warnings.push(
      `walk truncated at maxDepth=${maxDepth}` +
        (opts.identify
          ? ''
          : ' — no identify() was supplied, so a cyclic topology cannot terminate on its own (#406)'),
    );
  }

  return {
    nodes: [...byId.values()].map((n) => ({ id: n.id, routes: n.routes, identity: n.identity })),
    edges,
    busLinks,
    root,
    truncated,
    authoritative: !anyUnidentified,
    warnings,
  };
}
