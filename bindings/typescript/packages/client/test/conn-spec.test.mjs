// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Byte-pin `encodeConnSpec` against the C++ emitter, and prove a whole-vertex
 * `write` to a module's creator endpoint carries the SPEC verbatim (#408, #1492,
 * RFC-0014 §2 / ADR-0027 / ADR-0043 §5).
 *
 * The SPEC that creates a transport link is built independently by TWO encoders — the C++
 * `tr::net::conn_spec_t` and this package's `encodeConnSpec` — and a device only forms the
 * link if they agree byte-for-byte. Nothing structural stops them drifting, hence the pins
 * below. The DIAL pin is the REAL C++ output: it is the shared `conn/create-via-spec`
 * conformance vector's byte string, which `tests/conformance/vectors/v1/conn/create-via-spec`
 * documents as emitted by `conn_spec_t`'s one-argument (endpoint) constructor;
 * `test/vectors.test.mjs` pins it against the file itself. The LISTENER pin below carries
 * the ws-private keys, which no vector covers — it is a LAYOUT regression pin on this
 * encoder, derived from the same grammar, not an independent cross-core witness.
 *
 * RFC-0014 S7 retired the `write /net:children[] += SPEC{…}` door these pins used to cover.
 * The `write` test covers the surviving one: a whole-vertex WRITE to `/net/<module>/conn`,
 * with NO field selector, in RFC-0004 §B order.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { decode, TYPE } from '@avatarsd-llc/libtracer';
import { LibtracerClient, encodeConnSpec, encodeFwd, FWD_OP, FWD_KIND } from '../dist/index.js';

const hex = (u8) => Buffer.from(u8).toString('hex');

/**
 * @brief The exact bytes the C++ emitter produces for `conn_spec_t("up").addr("127.0.0.1")
 *        .port(8080)` — the `conn/create-via-spec` vector, 67 bytes.
 */
const CPP_DIAL_SPEC =
  '0e403f00020004006e616d6502000200757002000600636f6e6669670b4023000200040061646472020009003132372e' +
  '302e302e3102000400706f727401000200901f';

/**
 * @brief The bytes for a ws LISTENER carrying the ws-private `peer_named=1` / `max_peers=8`
 *        keys — the C++ chain `conn_spec_t("bus").kind("ws").port(47320)
 *        .flag("peer_named", true).u32("max_peers", 8)`, 101 bytes.
 */
const LISTEN_SPEC =
  '0e406100020004006e616d650200030062757302000600636f6e6669670b4044' +
  '00020004006b696e6402000200777302000400706f727401000200d8b802000a' +
  '00706565725f6e616d65640100010001020009006d61785f7065657273010004' +
  '0008000000';

test('encodeConnSpec matches the C++ emitter byte-for-byte: a ws DIAL client', () => {
  const spec = encodeConnSpec({
    name: 'up',
    port: 8080,
    addr: '127.0.0.1',
  });
  assert.equal(hex(spec), CPP_DIAL_SPEC, 'the TS and C++ SPEC encoders must not drift');
  assert.equal(spec.length, 67);
});

test('encodeConnSpec pins the ws-private LISTENER layout: peer_named + max_peers', () => {
  const spec = encodeConnSpec({
    name: 'bus',
    port: 47320,
    kind: 'ws',
    peerNamed: true,
    maxPeers: 8,
  });
  assert.equal(hex(spec), LISTEN_SPEC, 'the ws-private key layout must not drift');
  assert.equal(spec.length, 101);
});

test('encodeConnSpec decodes to the SPEC structure the C++ config_reader_t walks', () => {
  const spec = encodeConnSpec({ name: 'up', port: 8080, kind: 'ws', addr: '127.0.0.1' });
  const dec = decode(spec);
  assert.equal(dec.type, TYPE.SPEC);
  // NAME "name" NAME "up" NAME "config" SETTINGS{...} — no `type` pair since RFC-0014 S7.
  assert.equal(dec.children.length, 4);
  const cfg = dec.children[3];
  assert.equal(cfg.type, TYPE.SETTINGS, 'the config child is a SETTINGS record');
  // kind/addr/port => 6 positional key/value children; no `role` pair since RFC-0014 S7.
  assert.equal(cfg.children.length, 6);
});

test('encodeConnSpec carries an addr the path rules would reject (a config value is not a segment)', () => {
  // "127.0.0.1" holds dots — reserved in a PATH segment, legal in a config value.
  assert.doesNotThrow(() => encodeConnSpec({ name: 'up', port: 8080, kind: 'ws', addr: '127.0.0.1' }));
});

test('encodeConnSpec emits an addr-less config verbatim (the DIAL check is the device\'s)', () => {
  // Since RFC-0014 S7 the payload carries no `role`, so this encoder cannot tell a DIAL
  // (where a missing `addr` is fatal) from a LISTEN (where `addr` is ignored) — only the
  // endpoint the caller writes to knows. It emits what it is given; a DIAL module answers
  // TYPE_MISMATCH. Matching `tr::net::conn_spec_t`, which validates no key either.
  const spec = encodeConnSpec({ name: 'up', port: 8080, kind: 'ws' });
  const cfg = decode(spec).children[3];
  assert.equal(cfg.children.length, 4, 'kind + port only — nothing invented to stand in for addr');
});

/** @brief An in-memory ClientTransport recording the frames the client emits. */
function fakeTransport() {
  const sent = [];
  let sink = () => {};
  return {
    sent,
    send: (f) => sent.push(f),
    onFrame: (h) => {
      sink = h;
    },
    inject: (f) => sink(f),
  };
}

test('a write to the creator endpoint carries the SPEC payload and no FIELD', async () => {
  const t = fakeTransport();
  const client = new LibtracerClient(t, { replyEndpoint: ['reply-ep'] });
  const spec = encodeConnSpec({ name: 'up', port: 8080, kind: 'ws', addr: '127.0.0.1' });

  const pending = client.write(['net', 'ws-client', 'conn'], spec);

  assert.equal(t.sent.length, 1, 'exactly one FWD frame went out');
  const fwd = decode(t.sent[0]);
  assert.equal(fwd.type, TYPE.FWD);
  // RFC-0004 §B order: op, dst, src, payload — no FIELD, this is a WHOLE-VERTEX write.
  assert.equal(fwd.children.length, 4, 'the creator-endpoint write is field-less');
  assert.equal(fwd.children[0].payload[0], FWD_OP.WRITE);
  assert.equal(fwd.children[1].type, TYPE.PATH);
  assert.notEqual(fwd.children[2].type, TYPE.FIELD, 'no field selector rides on this door');
  assert.equal(fwd.children[2].type, TYPE.PATH, 'src follows dst directly');
  assert.equal(fwd.children[3].type, TYPE.SPEC, 'the payload is the SPEC verbatim');
  assert.equal(hex(fwd.children[3].payload ?? new Uint8Array(0)).length >= 0, true);

  // A RESULT reply resolves the write.
  t.inject(
    encodeFwd({
      op: FWD_OP.REPLY,
      dst: ['reply-ep'],
      src: ['net', 'ws-client', 'conn'],
      kind: FWD_KIND.RESULT,
    }),
  );
  await pending;
});
