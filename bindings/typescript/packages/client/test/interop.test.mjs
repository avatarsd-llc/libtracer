// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// #56 / RFC-0004 — END-TO-END client interop: the TypeScript LibtracerClient
// drives all five path-addressed operations over real `FWD` frames against a live
// C++ graph-backed node (`fwd_node_server`) over a genuine ws socket, and decodes
// the source-routed `FWD{REPLY}` / delivery. This is the web-UI<->device proof:
//   - read           -> RESULT carries the seeded VALUE
//   - write + read    -> the value is stored and reads back
//   - subscribe       -> the node pushes a producer VALUE the handler delivers
//   - readField :subscribers[] -> a POINT wrapper of the registered SUBSCRIBER
//   - await (timeout) -> a typed FwdError(TIMEOUT) when the deadline elapses
//
// GUARDED on LIBTRACER_FWD_NODE_SERVER pointing at the built `fwd_node_server`
// binary (a CMake target). Plain `npm test` without it SKIPS gracefully; the CI
// `fwd-interop` job builds the binary and sets the env var. No fixed sleeps: the
// PORT= line and every reply/delivery are awaited behind deadlines.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { WebSocket } from 'ws';
import { TransportWs } from '@avatarsd-llc/libtracer-ws';
import { TYPE } from '@avatarsd-llc/libtracer';
import { LibtracerClient, FwdError, encodeValue } from '../dist/index.js';

const SERVER = process.env.LIBTRACER_FWD_NODE_SERVER;
const skip = !SERVER || !existsSync(SERVER);

// Constants the C++ harness pins (keep in sync with fwd_node_server.cpp).
const SEEDED_TEMP = 0x1234abcd;
const PUSHED_SAMPLE = 0xcafebabe;
const WRITTEN = 0x00c0ffee;

/** Little-endian u32 bytes. */
function le32(v) {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, v >>> 0, true);
  return b;
}

/** @param {Uint8Array} a @param {Uint8Array} b */
function sameBytes(a, b) {
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

/**
 * Spawn the C++ fwd_node_server and resolve `{ child, port }` once its `PORT=<n>`
 * line lands on stdout, behind a deadline. Rejects (and kills) if the line never
 * arrives or the process dies first.
 */
function startServer(deadlineMs = 8000) {
  return new Promise((resolve, reject) => {
    const child = spawn(SERVER, ['--port', '0', '--timeout-ms', '20000'], {
      stdio: ['ignore', 'pipe', 'inherit'],
    });
    let buf = '';
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill('SIGKILL');
      reject(new Error('fwd_node_server did not print PORT= within deadline'));
    }, deadlineMs);

    child.stdout.on('data', (chunk) => {
      buf += chunk.toString('utf8');
      const m = buf.match(/PORT=(\d+)/);
      if (m && !settled) {
        settled = true;
        clearTimeout(timer);
        resolve({ child, port: Number(m[1]) });
      }
    });
    child.on('exit', (code) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(new Error(`fwd_node_server exited early (code ${code})`));
    });
    child.on('error', (err) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(err);
    });
  });
}

/** Resolve once the child has fully exited, behind a deadline. */
function waitExit(child, ms = 4000) {
  return new Promise((resolve) => {
    if (child.exitCode !== null || child.signalCode !== null) {
      resolve();
      return;
    }
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      resolve();
    }, ms);
    child.once('exit', () => {
      clearTimeout(timer);
      resolve();
    });
  });
}

test(
  'TS LibtracerClient drives read/write/await/readField/subscribe over FWD against a live C++ node',
  { skip: skip ? 'set LIBTRACER_FWD_NODE_SERVER to the built fwd_node_server binary' : false },
  async () => {
    const { child, port } = await startServer();
    const transport = new TransportWs(`ws://127.0.0.1:${port}`, { WebSocket });
    await transport.connect();
    const client = new LibtracerClient(transport);

    try {
      // 1) read — RESULT carries the seeded VALUE byte-exact.
      const read1 = await client.read('/sensor/temp');
      assert.equal(read1.type, TYPE.VALUE);
      assert.ok(sameBytes(read1.payload, le32(SEEDED_TEMP)), 'seeded read');

      // 2) write then read-back — the value is stored on the live vertex.
      await client.write('/sensor/temp', encodeValue(le32(WRITTEN)));
      const read2 = await client.read('/sensor/temp');
      assert.ok(sameBytes(read2.payload, le32(WRITTEN)), 'written value reads back');

      // 3) subscribe — the node pushes ONE producer VALUE the handler delivers.
      //    (handler registered before the ack, so the push is never dropped.)
      let resolveDelivery;
      const delivered = new Promise((res, rej) => {
        const timer = setTimeout(() => rej(new Error('no producer delivery within deadline')), 5000);
        resolveDelivery = (v) => {
          clearTimeout(timer);
          res(v);
        };
      });
      const unsubscribe = await client.subscribe('/sensor/temp', (value) => resolveDelivery(value));
      const sample = await delivered;
      assert.ok(sameBytes(sample, le32(PUSHED_SAMPLE)), 'live subscribe delivery');
      unsubscribe();

      // 4) readField :subscribers[] — a POINT wrapper holding the registered SUBSCRIBER.
      const subs = await client.readField('/sensor/temp', ':subscribers[]');
      assert.equal(subs.type, TYPE.POINT, ':subscribers[] read returns a POINT wrapper');
      assert.ok(subs.children.length >= 1, 'the registered subscriber is present');

      // 5) await (timeout) — no concurrent writer, so the deadline elapses and the
      //    responder replies kind=ERROR(TIMEOUT), surfaced as a typed FwdError.
      await assert.rejects(
        client.await_('/sensor/temp', 200_000_000n), // 200 ms
        (err) => err instanceof FwdError && err.codeName === 'TIMEOUT',
      );
    } finally {
      await transport.close();
      child.kill('SIGTERM');
      await waitExit(child);
    }
  },
);
