// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// C++ <-> TypeScript transport_ws INTEROP test (#54). Unlike roundtrip.test.mjs
// (which echoes against a pure-Node `ws` server), this drives the real C++
// `tr::net::transport_ws_server` over a live socket: a genuine RFC 6455 101
// handshake, a MASKED TS client BINARY frame in, an UNMASKED C++ server BINARY
// frame back — end-to-end validation of the web-UI <-> device path.
//
// It is GUARDED on the LIBTRACER_WS_INTEROP_SERVER env var pointing at the built
// `ws_interop_server` binary (CMake target). Plain `npm test` without it SKIPS
// gracefully rather than failing — the CI `ws-interop` job builds the binary and
// sets the env var. No fixed sleeps: the PORT= line, the frame event, and the
// child's stdout are all awaited behind deadlines.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { WebSocket } from 'ws';
import { TransportWs } from '../dist/index.js';
import { encode, decode, equal, TYPE } from '@avatarsd-llc/libtracer';

const SERVER = process.env.LIBTRACER_WS_INTEROP_SERVER;
const skip = !SERVER || !existsSync(SERVER);

/** A simple opaque NAME TLV built via the cross-validated core codec. */
function buildTlv(text) {
  return {
    type: TYPE.NAME,
    opt: { pl: false, ts: false, cr: false, ll: false, cw: false, tf: false },
    payload: new TextEncoder().encode(text),
    children: [],
    trailer: null,
  };
}

/**
 * Spawn the C++ ws_interop_server and resolve with `{ child, port }` once its
 * `PORT=<n>` line lands on stdout, behind a deadline. Rejects (and kills the
 * child) if the line never arrives or the process dies first.
 */
function startServer(deadlineMs = 8000) {
  return new Promise((resolve, reject) => {
    // --timeout-ms is the unconditional CI backstop; we kill the child sooner.
    const child = spawn(SERVER, ['--port', '0', '--timeout-ms', '15000'], {
      stdio: ['ignore', 'pipe', 'inherit'],
    });

    let buf = '';
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill('SIGKILL');
      reject(new Error('ws_interop_server did not print PORT= within deadline'));
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
      reject(new Error(`ws_interop_server exited early (code ${code})`));
    });
    child.on('error', (err) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(err);
    });
  });
}

/** Resolve once the child process has fully exited, behind a deadline. */
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
  'TLV round-trips byte-identical: TS TransportWs <-> C++ transport_ws_server over a real socket',
  { skip: skip ? 'set LIBTRACER_WS_INTEROP_SERVER to the built ws_interop_server binary' : false },
  async () => {
    const { child, port } = await startServer();

    const tlv = buildTlv('device-42');
    const frame = encode(tlv);

    const transport = new TransportWs(`ws://127.0.0.1:${port}`, { WebSocket });

    const received = new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('no echo from C++ server within deadline')), 5000);
      transport.onFrame((bytes) => {
        clearTimeout(timer);
        resolve(bytes);
      });
    });

    try {
      // A real RFC 6455 101 handshake against the C++ transport_ws_server.
      await transport.connect();
      // One MASKED client BINARY frame out; the C++ server unmasks and echoes it
      // back as one UNMASKED server BINARY frame.
      transport.send(frame);
      const got = await received;

      assert.deepEqual([...got], [...frame], 'echoed bytes are byte-identical across the socket');
      assert.ok(equal(decode(got), tlv), 'received bytes decode to an equal TLV');
    } finally {
      await transport.close();
      // The server self-exits at --timeout-ms; kill it now so the test never waits.
      child.kill('SIGTERM');
      await waitExit(child);
    }
  },
);
