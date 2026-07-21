<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# Wireshark dissector for libtracer

A single-file Lua dissector that decodes libtracer frames live in Wireshark:
the `type`/`opt`/`length` header, the `opt` bitfield, the optional wire-time and
CRC trailer (**verified**, mismatches raised as expert-info), `opt.PL=1` nested
recursion, PATH reconstruction to `/a/b/c`, and the `FWD`/`FIELD` remote-operation
frames.

> **Wire format is DRAFT** ([docs/spec/v1.md](../../docs/spec/v1.md) line 3). This
> dissector tracks the reference docs and is regression-tested against
> [`tests/conformance/vectors/v1/`](../../tests/conformance/vectors). If a vector's
> bytes change, update the dissector in the same PR.

## Install

1. Find your **personal Lua plugins** folder: Wireshark → *Help → About Wireshark
   → Folders → "Personal Lua Plugins"*. Typically:
   - Linux/macOS: `~/.local/lib/wireshark/plugins/` (or `~/.config/wireshark/plugins/`)
   - Windows: `%APPDATA%\Wireshark\plugins\`
2. Copy `libtracer.lua` there.
3. Reload plugins with **Ctrl+Shift+L** (Analyze → Reload Lua Plugins), or restart
   Wireshark. (Lua must be enabled — it is in the standard builds.)

## Which traffic it decodes

libtracer has no port field or per-frame version (identity lives at the discovery
layer), so binding is by transport. Ports are **preferences** — *Edit → Preferences
→ Protocols → libtracer*:

| Transport | How it binds | Default |
| --- | --- | --- |
| **WebSocket** (strawberry-fw node) | subdissector on the WS payload for the configured TCP port | port **80** |
| **Raw TCP** (`length_prefix_framer`) | `tcp.port` table; exact PDU reassembly via the fixed-width length | disabled (set the port) |
| **Any TCP/WS** | conservative heuristic (known type code, reserved bits zero, and either a verified CRC or an exact frame fit) | on |
| **QUIC / WebTransport** | inner bytes are inside TLS 1.3 — set `SSLKEYLOGFILE` on the endpoint and point *Preferences → TLS → (Pre)-Master-Secret log* at it, then the decrypted payload flows through the heuristic | needs keylog |
| **CAN** | not yet — a single TLV spans several ≤8-byte frames (reassembly). Planned. | — |

You can also right-click any frame → **Decode As…** → *libtracer* to force it on a
stream the heuristic didn't claim.

### Try it immediately

Generate a small demo capture of assorted frames and open it:

```sh
python3 tools/wireshark/tests/make_sample_pcap.py   # writes tools/wireshark/sample.pcap
wireshark tools/wireshark/sample.pcap
```

The CRC-bearing frames are claimed by the heuristic on sight; for the rest set the
*Raw TCP port* preference to `47301` (the port the demo uses) or *Decode As…*.

## Useful display filters

```
libtracer                       all libtracer frames
libtracer.type == 0x0f          FWD frames
libtracer.fwd.op == 1           FWD WRITEs (READ=0 WRITE=1 AWAIT=2 REPLY=3)
libtracer.fwd.kind == 1         FWD REPLYs carrying an ERROR (RESULT=0 ERROR=1)
libtracer.path contains "temp"  frames addressing a path with "temp"
libtracer.opt.cr == 1           frames carrying a CRC
libtracer.crc.bad               frames whose trailer CRC failed to verify
libtracer.error                 ERROR frames (shows tr::concept::name)
```

## Tests

The byte-walking logic is regression-tested against the conformance vectors — the
same `input.bin`/`expected.json` corpus the C++/Rust/TS cores use. The harness runs
the **actual** dissector (no reimplementation) through its `--decode-json` CLI:

```sh
# needs a Lua runtime: `apt install lua5.4`, or `pip install lupa`
python3 tools/wireshark/tests/run_tests.py -v
```

It checks type/opt/length/payload/children against each vector, requires every
CRC to verify, and requires the reserved-bit reject vectors to be flagged INVALID.
CI runs this on every change under `tools/wireshark/**` or the vectors
(`.github/workflows/wireshark-dissector.yml`).

## Standalone decode

The dissector doubles as a CLI for quick inspection or scripting:

```sh
$ lua tools/wireshark/libtracer.lua --decode-json 09000000
{"summary":"STATUS OK","frame":{...}}
```
