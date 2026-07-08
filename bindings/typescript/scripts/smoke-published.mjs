// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Published-package smoke: run against a FRESH install of the packed tarball (not
// the workspace), so it proves the published @avatarsd-llc/libtracer actually
// imports — that `dist/` shipped, the `exports` map resolves, and the public
// surface is present. Catches the "published package is missing files / broken
// exports" class of bug (the same failure mode as the ESP component archive)
// that the workspace `npm test` cannot see. Wired into ts.yml.
import assert from "node:assert";
import * as lt from "@avatarsd-llc/libtracer";
import * as wire from "@avatarsd-llc/libtracer/wire";

const need = ["decode", "encode", "equal", "crc32c", "crc16ccitt", "TYPE", "ERROR", "SPEC_VERSION"];
for (const k of need) assert.ok(k in lt, `@avatarsd-llc/libtracer is missing export: ${k}`);
assert.strictEqual(typeof lt.decode, "function", "decode is not a function");
assert.strictEqual(typeof lt.encode, "function", "encode is not a function");
assert.strictEqual(lt.SPEC_VERSION, 1, "SPEC_VERSION != 1");
assert.ok("decode" in wire, "@avatarsd-llc/libtracer/wire subpath is missing decode");

console.log(`published-package smoke ok: ${need.length} exports + /wire subpath resolve from the tarball`);
