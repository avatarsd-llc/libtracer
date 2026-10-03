// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @file
 * @brief A stand-in for the remote node the client examples talk to. NOT an example.
 *
 * The TypeScript packages are a codec and a client: there is no TS node to answer
 * a request. So that each example can stay on its one concept and still run with no
 * C++ build, this answers the three operations they use, in memory: READ (or
 * `NOT_FOUND`), WRITE (store, then deliver to subscribers) and a `:subscribers[]`
 * write. Against a real node the same client code runs unchanged.
 */

import { encode, TYPE } from '@avatarsd-llc/libtracer';
import { decodeFwd, encodeFwd, pathSegments, FWD_OP, FWD_KIND, FWD_ERROR } from '@avatarsd-llc/libtracer-client';

/** @brief A TLV node literal: structured when it has children. */
const node = (type, payload, children = []) => ({
  type,
  opt: { pl: children.length > 0, ts: false, cr: false, ll: false, cw: false, tf: false },
  payload: children.length > 0 ? new Uint8Array(0) : payload,
  children,
  trailer: null,
});

/** @brief `STATUS{ ERROR{ VALUE u16 code } }` — the payload of a `kind=ERROR` reply. */
const errorStatus = (code) =>
  encode(node(TYPE.STATUS, null, [node(TYPE.ERROR, null, [node(TYPE.VALUE, Uint8Array.of(code & 0xff, code >> 8))])]));

/** @brief Create an empty stand-in node. */
export function standInNode() {
  const store = new Map(); // '/sensor/temp' -> encoded VALUE
  const subscribers = new Map(); // '/sensor/temp' -> [{ target, send }]

  /** @brief Answer one inbound frame from a peer; `send` puts a frame back on that peer's link. */
  function handle(bytes, send) {
    const req = decodeFwd(bytes);
    const dst = pathSegments(req.dst);
    const key = '/' + dst.join('/');
    const reply = (kind, payload) =>
      send(encodeFwd({ op: FWD_OP.REPLY, dst: pathSegments(req.src), src: dst, kind, payload }));

    if (req.op === FWD_OP.READ) {
      const value = store.get(key);
      return value ? reply(FWD_KIND.RESULT, value) : reply(FWD_KIND.ERROR, errorStatus(FWD_ERROR.NOT_FOUND));
    }
    if (req.op === FWD_OP.WRITE && req.field) {
      const target = pathSegments(req.payload.children.find((c) => c.type === TYPE.PATH));
      subscribers.set(key, [...(subscribers.get(key) ?? []), { target, send }]);
      return reply(FWD_KIND.RESULT);
    }
    if (req.op === FWD_OP.WRITE) {
      const value = encode(req.payload);
      store.set(key, value);
      reply(FWD_KIND.RESULT);
      for (const sub of subscribers.get(key) ?? [])
        sub.send(encodeFwd({ op: FWD_OP.WRITE, dst: sub.target, src: dst, payload: value }));
    }
  }

  /** @brief An in-memory `ClientTransport` wired straight to this node. */
  function loopback() {
    let receiver = null;
    return {
      send: (frame) => queueMicrotask(() => handle(frame, (b) => receiver?.(b))),
      onFrame: (r) => (receiver = r),
    };
  }

  return { handle, loopback };
}
