// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @file
 * @brief One concept: a remote failure is a reply, and the client surfaces it as a typed
 * `FwdError` carrying the registered wire code — never as a hang or a bare string.
 *
 * Run: `node examples/remote-error.mjs` (from `bindings/typescript/`, after `npm run build`).
 */

import assert from 'node:assert/strict';
import { LibtracerClient, FwdError, FWD_ERROR } from '@avatarsd-llc/libtracer-client';
import { standInNode } from './stand-in-node.mjs';

const client = new LibtracerClient(standInNode().loopback());

// Nothing was ever written at /sensor/missing, so the node answers FWD{REPLY, kind=ERROR}.
try {
  await client.read('/sensor/missing');
  assert.fail('a read of an absent vertex must reject');
} catch (err) {
  assert.ok(err instanceof FwdError);
  assert.equal(err.code, FWD_ERROR.NOT_FOUND);
  console.log(`read /sensor/missing -> ${err.codeName} (0x${err.code.toString(16).padStart(4, '0')})`);
}
