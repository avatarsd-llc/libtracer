<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# Wireshark dissector for libtracer

A single-file Lua dissector that decodes libtracer frames live in Wireshark:
the `type`/`opt`/`length` header, the `opt` bitfield, the optional wire-time and
CRC trailer (**verified**, mismatches raised as expert-info), `opt.PL=1` nested
recursion, PATH reconstruction to `/a/b/c` — including RFC-0027 **path labels**,
decoded to their slot index and generation — and the `FWD`/`FIELD` remote-operation
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
| **WebSocket** (a node serving the graph over WS) | subdissector on the WS payload for the configured TCP port | port **80** |
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
libtracer.path.label            frames whose address carries a path label
libtracer.path.label.index == 3 frames addressing label slot 3 (on the minting host)
libtracer.opt.cr == 1           frames carrying a CRC
libtracer.crc.bad               frames whose trailer CRC failed to verify
libtracer.error                 ERROR frames (shows tr::concept::name)
libtracer.field.selector        the FIELD selector in source spelling
```

### Reading a path label

A `PATH` element may be a **path label** ([RFC-0027](../../docs/spec/rfcs/0027-label-switched-path-compression.md)):
a hop's whole local part, replaced on a reply leg by the 32-bit alias that hop
minted for it. On the wire it is an RFC-0018 escape record — `00 16 04 <u32 LE>`,
7 bytes — sitting where a name segment would sit, so the dissector renders it in
place, inside the address:

| Rendering | What it is |
| --- | --- |
| `<label:3@7>` | slot index 3 at generation 7 — a well-formed label |
| `<label:3@0 UNMINTED>` | generation `0`, the reserved "no label"; never legitimately on the wire, refused **where it is dereferenced** with `tr::path::not_found` |
| `<label:bad-len=3>` | kind `0x16` with a length that is not 4 — a malformed *address* (`tr::path::invalid`), deliberately **not** read as a label |
| `<esc:17=AABB>` | an escape at some other kind: not a label, whatever its length. A relaying hop steps over it by its declared length |

Two things about `<label:N@G>` that a capture will not tell you and that change
how it must be read:

- **It is host-scoped.** The bytes name a vertex only on the host that minted
  them. The same `<label:3@7>` in two captures — or in the `dst` and `src` of one
  frame — is two unrelated things. There is no global name here to correlate on.
- **The generation is the whole staleness mechanism.** When the vertex behind a
  slot departs, the minting host bumps that slot's generation; the label the peer
  still holds then compares unequal and earns a `tr::path::not_found`, with the
  sender falling back to the string address it never threw away. So a peer sending
  a *lower* generation than the host's current one is a peer about to be refused —
  which is what `libtracer.path.label.generation` is for.

The frame is codec-valid either way: a packed `PATH` body is `opt.PL = 0` and
opaque to the codec, so a malformed label raises the *Malformed path label* expert
info (`libtracer.path.label.invalid`) and never flags the frame itself invalid.

### Telling the four FIELD forms apart

`libtracer.field.selector` renders a FIELD selector the way you would write it,
including the 1-byte `index_mode` (RFC-0004 §C) that decides what the frame
actually does:

| Selector | `index_mode` | Meaning |
| --- | --- | --- |
| `:subscribers` | absent ⇒ `SCALAR` | the whole array |
| `:subscribers[3]` | `ELEMENT` + index | one slot |
| `:subscribers[]` | `ELEMENT`, no index | append (subscribe) |
| `:subscribers[*]` | `WILDCARD` | every slot |

`[]` and `[*]` differ **by one byte on the wire** and by a great deal in effect,
so filter on the selector, not `libtracer.field.name` (which is still the plain
first-level name). Nested selectors join with `.` — `:settings.app.kp`. An
`index_mode` byte outside `{0,1,2}` renders as `[mode?N]` and raises the
malformed expert info; the reference resolver rejects it with `INVALID_PATH`.

## Tests

The byte-walking logic is regression-tested against the conformance vectors — the
same `input.bin`/`expected.json` corpus the C++/Rust/TS cores use. The harness runs
the **actual** dissector (no reimplementation) through its `--decode-json` CLI:

```sh
# needs a Lua runtime: `apt install lua5.4`, or `pip install lupa`
python3 tools/wireshark/tests/run_tests.py -v

# the Wireshark-SIDE half: ProtoFields, expert infos and the tree walk, against a
# stubbed Proto/ProtoField/Tvb (needs a Lua binary, not lupa)
lua tools/wireshark/tests/tree_smoke.lua
```

`run_tests.py` checks type/opt/length/payload/children against each vector,
requires every CRC to verify, and requires the reserved-bit reject vectors to be
flagged INVALID. `tree_smoke.lua` covers what it cannot: the registration block
runs only when a `Proto` global exists, so before it that half of the file was
held by `luac5.4 -p` alone — a syntax check, blind to a `uint16` field added from
a four-byte slice or a range that runs off the buffer. CI runs both on every
change under `tools/wireshark/**` or the vectors
(`.github/workflows/wireshark-dissector.yml`).

Rendered addresses are pinned in `run_tests.py` itself (`VECTOR_RENDERINGS`)
rather than in `expected.json`: the bytes are the shared corpus's, but
`<label:1@2>` is a display decision this tool makes and the other cores do not, so
it has no business constraining them. Two guards keep those pins honest — an
expectation naming a vector that no longer exists **fails** the run rather than
quietly not running, and `SYNTHETIC_FRAMES` carries the shapes no vector does
(generation `0`; a foreign escape kind whose payload is 4 bytes long, which is the
frame a length-only check mis-reads as a label).

## Standalone decode

The dissector doubles as a CLI for quick inspection or scripting:

```sh
$ lua tools/wireshark/libtracer.lua --decode-json 09000000
{"summary":"STATUS OK","frame":{...}}
```
