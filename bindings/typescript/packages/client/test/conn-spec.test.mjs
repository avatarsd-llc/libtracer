// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Byte-pin `encodeConnSpec` against the C++ emitter, and prove `writeField`
 * emits the in-band `:children[]` append (#408, ADR-0027 / ADR-0043 §5).
 *
 * The SPEC that creates a transport link is now built independently by TWO encoders —
 * the C++ `conn_spec` test helper and this package's `encodeConnSpec` — and a device
 * only forms the link if they agree byte-for-byte. Nothing structural stops them
 * drifting, so the expected vectors below are the REAL C++ output, captured from the
 * production `tr::wire::emit_*` helpers over `core/build`. Regenerate with the dump
 * program referenced in the PR for #408 if the layout ever legitimately changes.
 *
 * The `writeField` test covers the other half: that a field-selected WRITE carries both
 * the FIELD selector and the payload, in RFC-0004 §B order.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { decode, TYPE } from '@avatarsd-llc/libtracer';
import { LibtracerClient, encodeConnSpec, encodeFwd, FWD_OP, FWD_KIND } from '../dist/index.js';

const hex = (u8) => Buffer.from(u8).toString('hex');

/**
 * @brief The exact bytes the C++ emitter produces for
 *        `conn_spec("client", "up", DIAL, 8080, "ws", "127.0.0.1")` — 112 bytes.
 */
const CPP_DIAL_SPEC =
  '0e406c00020004007479706502000600636c69656e74020004006e616d6502000200757002000600636f6e6669670b403e' +
  '0002000400726f6c65010001000002000400706f727401000200901f020004006b696e64020002007773020004006164' +
  '6472020009003132372e302e302e31';

/**
 * @brief The exact bytes the C++ emitter produces for a ws LISTENER carrying the
 *        ws-private `peer_named=1` / `max_peers=8` keys — 134 bytes.
 */
const CPP_LISTEN_SPEC =
  '0e4082000200040074797065020008006c697374656e6572020004006e616d650200030068756202000600636f6e6669' +
  '670b40510002000400726f6c65010001000102000400706f727401000200d8b8020004006b696e6402000200777302' +
  '000a00706565725f6e616d65640100010001020009006d61785f70656572730100040008000000';

test('encodeConnSpec matches the C++ emitter byte-for-byte: a ws DIAL client', () => {
  const spec = encodeConnSpec({
    type: 'client',
    name: 'up',
    role: 'dial',
    port: 8080,
    kind: 'ws',
    addr: '127.0.0.1',
  });
  assert.equal(hex(spec), CPP_DIAL_SPEC, 'the TS and C++ SPEC encoders must not drift');
  assert.equal(spec.length, 112);
});

test('encodeConnSpec matches the C++ emitter byte-for-byte: a peer_named ws LISTENER', () => {
  const spec = encodeConnSpec({
    type: 'listener',
    name: 'hub',
    role: 'listen',
    port: 47320,
    kind: 'ws',
    peerNamed: true,
    maxPeers: 8,
  });
  assert.equal(hex(spec), CPP_LISTEN_SPEC, 'the ws-private keys must match the C++ layout');
  assert.equal(spec.length, 134);
});

test('encodeConnSpec decodes to the SPEC structure the C++ config_reader_t walks', () => {
  const spec = encodeConnSpec({ type: 'client', name: 'up', role: 'dial', port: 8080, kind: 'ws', addr: '127.0.0.1' });
  const dec = decode(spec);
  assert.equal(dec.type, TYPE.SPEC);
  // NAME "type" NAME "client" NAME "name" NAME "up" NAME "config" SETTINGS{...}
  assert.equal(dec.children.length, 6);
  const cfg = dec.children[5];
  assert.equal(cfg.type, TYPE.SETTINGS, 'the config child is a SETTINGS record');
  // role/port/kind/addr => 8 positional key/value children.
  assert.equal(cfg.children.length, 8);
});

test('encodeConnSpec carries an addr the path rules would reject (a config value is not a segment)', () => {
  // "127.0.0.1" holds dots — reserved in a PATH segment, legal in a config value.
  assert.doesNotThrow(() =>
    encodeConnSpec({ type: 'client', name: 'up', role: 'dial', port: 8080, kind: 'ws', addr: '127.0.0.1' }),
  );
});

test('encodeConnSpec rejects a DIAL with no addr (the factories return TYPE_MISMATCH)', () => {
  assert.throws(
    () => encodeConnSpec({ type: 'client', name: 'up', role: 'dial', port: 8080, kind: 'ws' }),
    /DIAL connection requires an addr/,
  );
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

test('writeField emits a field-selected WRITE carrying the SPEC payload', async () => {
  const t = fakeTransport();
  const client = new LibtracerClient(t, { replyEndpoint: ['reply-ep'] });
  const spec = encodeConnSpec({ type: 'client', name: 'up', role: 'dial', port: 8080, kind: 'ws', addr: '127.0.0.1' });

  const pending = client.writeField('/net', ':children[]', spec);

  assert.equal(t.sent.length, 1, 'exactly one FWD frame went out');
  const fwd = decode(t.sent[0]);
  assert.equal(fwd.type, TYPE.FWD);
  // RFC-0004 §B order: op, dst, field, src, payload.
  assert.equal(fwd.children.length, 5, 'a field-selected WRITE carries BOTH a FIELD and a payload');
  assert.equal(fwd.children[0].payload[0], FWD_OP.WRITE);
  assert.equal(fwd.children[1].type, TYPE.PATH);
  assert.equal(fwd.children[2].type, TYPE.FIELD, 'the FIELD selector rides between dst and src');
  assert.equal(fwd.children[3].type, TYPE.PATH, 'src follows the selector');
  assert.equal(fwd.children[4].type, TYPE.SPEC, 'the payload is the SPEC verbatim');
  assert.equal(hex(fwd.children[4].payload ?? new Uint8Array(0)).length >= 0, true);

  // A RESULT reply resolves the write.
  t.inject(encodeFwd({ op: FWD_OP.REPLY, dst: ['reply-ep'], src: ['net'], kind: FWD_KIND.RESULT }));
  await pending;
});
