// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief C++ <-> BROWSER WebTransport interop harness (ADR-0043 Phase B).
 *
 * Node has no native WebTransport client, so unlike transport-ws's interop
 * this cannot run in-process: it drives a real chrome-headless (via puppeteer)
 * against the C++ `webtransport_transport_t` echo harness (CMake target
 * `wt_interop_server`, built with LIBTRACER_WITH_QUIC): a genuine HTTP/3
 * extended-CONNECT session with `serverCertificateHashes` dev trust, one
 * browser `createBidirectionalStream()`, a length-prefixed record out, the
 * echoed record back — end-to-end validation of the browser <-> device path.
 *
 * It SKIPS gracefully (never fails) unless BOTH are present:
 *   - LIBTRACER_WT_INTEROP_SERVER: path to the built wt_interop_server binary;
 *   - puppeteer: installed (`npm i -D puppeteer` where the harness should run —
 *     it is deliberately NOT a devDependency; the mocked-WebTransport unit
 *     tests cover the transport logic, and core/tests/webtransport_test.cpp
 *     covers the wire end-to-end in C++).
 *
 * The dev certificate is generated here: `serverCertificateHashes` is only
 * honored by browsers for an ECDSA certificate valid <= 14 days (a browser
 * rule) — tools/gen-dev-cert.sh's RSA/365d pair would be rejected.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const SERVER = process.env.LIBTRACER_WT_INTEROP_SERVER;

let puppeteer = null;
try {
  ({ default: puppeteer } = await import('puppeteer'));
} catch {
  /* not installed — skip below with a clear message */
}

const skip =
  !SERVER || !existsSync(SERVER)
    ? 'set LIBTRACER_WT_INTEROP_SERVER to the built wt_interop_server binary'
    : !puppeteer
      ? 'puppeteer is not installed (npm i -D puppeteer to run the browser interop)'
      : false;

/** @brief Generate an ECDSA P-256 self-signed cert valid 10 days (the browser rule). */
function genBrowserDevCert(dir) {
  const r = spawnSync(
    'openssl',
    [
      'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1',
      '-sha256', '-days', '10', '-nodes',
      '-keyout', join(dir, 'key.pem'), '-out', join(dir, 'cert.pem'),
      '-subj', '/CN=libtracer-wt-dev',
      '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1',
    ],
    { stdio: 'ignore' },
  );
  assert.equal(r.status, 0, 'openssl generated the ECDSA dev cert');
  return { cert: join(dir, 'cert.pem'), key: join(dir, 'key.pem') };
}

/** @brief SHA-256 of the certificate's DER bytes (what serverCertificateHashes pins). */
function certSha256(certPem) {
  const pem = readFileSync(certPem, 'utf8');
  const b64 = pem
    .replace(/-----BEGIN CERTIFICATE-----/, '')
    .replace(/-----END CERTIFICATE-----/, '')
    .replace(/\s+/g, '');
  return createHash('sha256').update(Buffer.from(b64, 'base64')).digest();
}

/** @brief Spawn wt_interop_server; resolve `{ child, port }` on its PORT= line. */
function startServer(cert, key, deadlineMs = 8000) {
  return new Promise((resolve, reject) => {
    const child = spawn(
      SERVER,
      ['--port', '0', '--cert', cert, '--key', key, '--timeout-ms', '30000'],
      { stdio: ['ignore', 'pipe', 'inherit'] },
    );
    let buf = '';
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill('SIGKILL');
      reject(new Error('wt_interop_server did not print PORT= within deadline'));
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
      reject(new Error(`wt_interop_server exited early (code ${code})`));
    });
    child.on('error', (err) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(err);
    });
  });
}

test(
  'TLV frame round-trips byte-identical: browser WebTransport <-> C++ webtransport_transport_t',
  { skip },
  async () => {
    const dir = mkdtempSync(join(tmpdir(), 'libtracer-wt-interop-'));
    const { cert, key } = genBrowserDevCert(dir);
    const hash = certSha256(cert);
    const { child, port } = await startServer(cert, key);

    const browser = await puppeteer.launch({
      headless: 'new',
      args: ['--no-sandbox', '--disable-setuid-sandbox'],
    });
    try {
      const page = await browser.newPage();
      // Inside the browser: the raw WebTransport + the SAME 4-byte u32-LE
      // length-prefix framing TransportWebTransport speaks (inlined — the page
      // context cannot import the package; the unit tests pin the TS class to
      // this exact wire shape).
      const echoed = await page.evaluate(
        async (wtPort, hashBytes, frameBytes) => {
          const wt = new WebTransport(`https://127.0.0.1:${wtPort}/`, {
            serverCertificateHashes: [
              { algorithm: 'sha-256', value: new Uint8Array(hashBytes) },
            ],
          });
          await wt.ready;
          const stream = await wt.createBidirectionalStream();
          const writer = stream.writable.getWriter();
          const frame = new Uint8Array(frameBytes);
          const record = new Uint8Array(4 + frame.length);
          new DataView(record.buffer).setUint32(0, frame.length, true);
          record.set(frame, 4);
          await writer.write(record);

          const reader = stream.readable.getReader();
          const got = [];
          let need = 4 + frame.length;
          while (need > 0) {
            const { done, value } = await reader.read();
            if (done) break;
            got.push(...value);
            need -= value.length;
          }
          wt.close();
          return got.slice(4); // strip the echoed prefix
        },
        port,
        [...hash],
        [0x03, 0x01, 0x64, 0x2a], // an opaque little TLV-ish payload
      );
      assert.deepEqual(echoed, [0x03, 0x01, 0x64, 0x2a], 'echo is byte-identical');
    } finally {
      await browser.close();
      child.kill('SIGTERM');
    }
  },
);
