<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# connection config — the SPEC config keys (L4)

```{admonition} In one paragraph
:class: tip
Creating a connection is an ordinary write — `write /net:children[] += SPEC{type,
name, config}` — and everything the new link needs is in that `config` SETTINGS TLV.
Its keys come in two families. The **universal** ones (`kind`, `addr`, `port`,
`role`, …) are parsed centrally into `tr::net::conn_settings_t`, which every
transport kind shares. The **kind-private** ones are parsed by the selected kind's
own factory, module-side, and never land on that shared record — so `quic` reads
`cert`/`key`/`ca`/`insecure`, `ws` and `tcp` read `peer_named`/`max_peers`, `can`
reads its bus identity and ingress bounds, and none of them can see each other's
vocabulary. This page is the key-by-key reference for both families, and the two
keys most worth reading before you ship are `ca` and `insecure`: a SPEC-created
`quic`/`webtransport` dialer **verifies its peer's certificate by default**.
```

## What this page is, and what it is not

This is the reference home for the connection-creation config keys. Before it
existed, a key was discoverable only from the factory's own Doxygen block or from
`core/CHANGELOG.md` — including the two `quic` keys that decide whether a dialer
authenticates its peer at all.

It is not the creation *protocol*: the SPEC shape, the ACL gate on the append, the
`/net/<module>/<name>` mount and the liveness value all belong to
[fwd-router § the /net connection model](fwd-router.md). It is not the *walk*
either — the positional pair grammar and its forward-compat rules are
[config § runtime configuration](config.md). This page is the **vocabulary**: which
keys exist, which factory reads each one, what wire value each takes, and what it
does.

Its subject is deliberately bounded to what `tr::net::config_reader_t` reads. The
creation SPEC's own envelope (`type`, `name`, `config`), the SUBSCRIBER QoS
SETTINGS and the ACL `SETTINGS` walk read the same positional grammar without that
type — they sit at L4, where `tr::net` cannot be a dependency — and they are not
connection config.

## The shape of a config

A `config` value is a `SETTINGS` TLV whose children are positional
`(NAME key, value)` pairs. String-valued keys take a `NAME` child; integer and
boolean keys take a `VALUE` child holding a little-endian unsigned integer. Four
rules from the shared reader apply to every key on this page:

- **Unknown pairs are ignored**, whole — key *and* value — so a newer peer may send
  keys this node has never heard of.
- **A wrong-typed value is ignored** as though the key were absent; so is an empty
  `VALUE` payload.
- **A repeated key resolves to its last well-formed occurrence.**
- **A child that is not a `NAME` where a key belongs stops the walk.** Every key
  after that point reads as absent.

The consequence that bites: *there is no error return for a misspelled or
mistyped key.* `insecrue = 1` and `insecure = "1"` (a `NAME` value where a `VALUE`
belongs) both create a connection that silently took the default. The gate against
that is reading the table below, not a status code.

## Universal keys — parsed into `conn_settings_t`

Read once, centrally, for every kind (`core/src/transport_vertex.cpp`). A kind's
factory receives the parsed record alongside the raw config TLV.

<!-- config-keys:begin core/src/transport_vertex.cpp -->

| key | value | applies to | default | meaning |
| --- | --- | --- | --- | --- |
| `kind` | `NAME` utf-8 | both | empty | Selects the transport factory (`udp`, `tcp`, `ws`, or any kind registered through `register_transport_type`). Empty means "use the link staged by `provide_link`"; an unregistered kind fails creation with `SCHEMA_NOT_FOUND`. |
| `addr` | `NAME` utf-8 | DIAL | empty | Peer address, IPv4 dotted-quad. A DIAL with it empty answers `TYPE_MISMATCH` in all five socket factories — the three built-ins (`udp`, `tcp`, `ws`) share one precondition helper, and `quic`/`webtransport` repeat the check. `can` never reads it. |
| `port` | `VALUE` u16 | both | `0` | Peer port on a DIAL, bind port on a LISTEN. `0` answers `TYPE_MISMATCH` in the same five socket factories, on both roles. `can` never reads it. |
| `role` | `VALUE` u8 | both | the child type's default | `0` = DIAL, non-zero = LISTEN. Overrides the default the catalog type carries (`client` = DIAL, `listener` = LISTEN), and selects which declared module the connection mounts under. |
| `keepalive` | `VALUE` u32 | both | `0` | Keepalive interval in ms. **Nothing reads it**: `conn_settings_t::keepalive_ms` has no consumer outside the parse that fills it. UDP is connectionless, TCP has its own, WS handles PING/PONG at the protocol layer. |
| `max_frame` | `VALUE` u32 | both | `0` | Per-connection inbound frame cap in bytes, honoured by five of the six kinds — `tcp`, `quic` and `webtransport` read it off their u32 length prefix, `ws` off the RFC 6455 header, `udp` off the received datagram's length (one datagram = one frame). `0` = the 16 MiB protocol default on the framed kinds, and `udp_transport_t::kMaxDatagram` (64 KiB) on `udp`. On the four framed kinds the effective cap is `min(configured value, the injected backend's `max_segment_size()`)` and it does **not** only tighten: the default heap backend reports `SIZE_MAX`, so on a host a value above 16 MiB genuinely RAISES the ingress bound (#1035). On `udp` it can only tighten — a datagram cannot exceed 64 KiB, so a larger configured value is inert. `can` (its own fragmentation) does not read it. |
| `backoff` | `VALUE` u32 | DIAL | `0` | Self-heal retry interval in ms (RFC-0014 §4). **Parsed but dormant** — the liveness engine that would consume it is not implemented. |
| `connect_timeout` | `VALUE` u32 | DIAL | `0` | How long one dial attempt waits for `UP`, in ms (RFC-0014 §4). **Parsed but dormant**, same reason. |

<!-- config-keys:end -->

## Kind-private keys

Each kind's factory parses its own keys out of the raw config TLV. Nothing here is
a field on `conn_settings_t`, and no kind can read another kind's key.

### `tcp` — the LISTEN-side bus facet

<!-- config-keys:begin core/src/builtin_transport_tcp.cpp -->

| key | value | applies to | default | meaning |
| --- | --- | --- | --- | --- |
| `peer_named` | `VALUE` u8 (flag) | LISTEN | `0` | Non-zero exposes the `bus_link_t` facet (ADR-0044): each accepted peer gets its own return-route identity and the connection's `:children[]` enumerates live peers. Without it a listener is a **broadcast** link — `send` fans out to every open peer and no peer is individually addressable. |
| `max_peers` | `VALUE` u32 | LISTEN | `0` | Concurrent-peer admission cap. `0` = uncapped by this key (still bounded by the host's resources, per RFC-0006). |

<!-- config-keys:end -->

Both are ignored on a DIAL: a client has exactly one peer, itself.

### `ws` — the same two keys, verbatim

<!-- config-keys:begin core/src/builtin_transport_ws.cpp -->

| key | value | applies to | default | meaning |
| --- | --- | --- | --- | --- |
| `peer_named` | `VALUE` u8 (flag) | LISTEN | `0` | As `tcp`: the ADR-0044 per-peer identity facet instead of a broadcast link. |
| `max_peers` | `VALUE` u32 | LISTEN | `0` | As `tcp`: the concurrent-peer admission cap. |

<!-- config-keys:end -->

### `udp` — no kind-private keys

<!-- config-keys:begin core/src/builtin_transport_udp.cpp -->

The `udp` factory constructs no config reader at all: `addr`/`port`/`role`/`max_frame`
from the universal set are the whole of its configuration. A DIAL binds an ephemeral
local port and targets `addr:port`; a LISTEN binds `port` and learns its peer from the
first inbound datagram's source.

<!-- config-keys:end -->

This is a *checked* claim, not an omission — see [How this page is kept
true](#how-this-page-is-kept-true).

### `can` — bus identity and ingress bounds

Of the six transport factories in this tree, `can` is the only one that ignores
`conn_settings_t` outright — its lambda takes the record as an unnamed parameter — so
`addr`, `port`, `keepalive` and `max_frame` do nothing on a `can` connection. (`kind` and `role` still matter — they are consumed
by the connection vertex itself, to select this factory and to resolve the module
the vertex mounts under.)

<!-- config-keys:begin core/src/transport_can.cpp -->

| key | value | applies to | default | meaning |
| --- | --- | --- | --- | --- |
| `ifname` | `NAME` utf-8 | both | — (required) | The SocketCAN interface (`can0`, `vcan0`). Empty answers `TYPE_MISMATCH`; an interface the kernel will not open answers `TRANSPORT_DOWN`. |
| `node` | `VALUE` u16 | both | — (required) | This node's id, the `node` band of the 29-bit CAN ID. Required — an absent key answers `TYPE_MISMATCH`, and so does a value above `8191` (13 bits). |
| `version` | `VALUE` u8 | both | `0` | Protocol-version prefix, the top 4 bits of the CAN ID, so distinct versions occupy disjoint arbitration bands. Above `15` answers `TYPE_MISMATCH`. |
| `path` | `NAME` utf-8 | both | empty | The path advertised for this node's groups. Same spelling as the `webtransport` kind's `path`, unrelated meaning — that one is an HTTP URL path. Kind-private keys cannot collide at parse time; the collision is in the reader's head. |
| `fd` | `VALUE` u8 (flag) | both | `0` | Non-zero selects CAN-FD framing (≤64 B data fields) instead of classic (≤8 B). |
| `peer_ttl_ms` | `VALUE` u32 | both | `3000` | Peer liveness window (ADR-0044): a peer silent longer than this leaves the enumeration. |
| `max_groups` | `VALUE` u32 | both | `0` | Live reassembly-group ceiling. `0` = uncapped by this key; overflow evicts the oldest group and ticks `dropped_groups`. |
| `max_pending` | `VALUE` u32 | both | `0` | Ceiling on data slices parked awaiting their advertise. `0` = uncapped by this key; overflow evicts the oldest and ticks `dropped_rx`. |
| `rx_ttl_ms` | `VALUE` u32 | both | `0` | RX staleness window: a parked slice or an incomplete group untouched this long is reclaimed, so a lost advertise cannot pin one forever. `0` means *track `peer_ttl_ms`* — never "disabled". |

<!-- config-keys:end -->

The two count caps and the `pmr` resource behind them are the ingress-bounding seam;
the byte-level story is on [can](can.md).

### `quic` and `webtransport` — the TLS material

Two modules, one identical key set, both in the separate `libtracer_quic` target.
Two keys are the LISTEN-side served credential; two are the DIAL-side trust
decision.

<!-- config-keys:begin core/src/transport_quic.cpp -->

| key | value | applies to | default | meaning |
| --- | --- | --- | --- | --- |
| `cert` | `NAME` utf-8 | LISTEN | — (required) | PEM server-certificate path. Absent answers `TYPE_MISMATCH`; a path msquic will not load answers `TRANSPORT_DOWN` (the listener did not come up). |
| `key` | `NAME` utf-8 | LISTEN | — (required) | PEM private-key path matching `cert`. Same two failures. |
| `ca` | `NAME` utf-8 | DIAL | empty ⇒ the system trust store | PEM CA-bundle the peer's certificate is verified against, *instead of* the system trust store. |
| `insecure` | `VALUE` u8 (flag) | DIAL | `0` | **DEV ONLY.** Non-zero skips server-certificate validation entirely. |

<!-- config-keys:end -->

`webtransport` reads the same four, with the same meanings, plus one key `quic`
has no use for — it is the only kind here with an HTTP layer, so it is the only
one with a resource to name:

<!-- config-keys:begin core/src/transport_webtransport.cpp -->

| key | value | applies to | default | meaning |
| --- | --- | --- | --- | --- |
| `cert` | `NAME` utf-8 | LISTEN | — (required) | PEM server-certificate path. |
| `key` | `NAME` utf-8 | LISTEN | — (required) | PEM private-key path matching `cert`. |
| `ca` | `NAME` utf-8 | DIAL | empty ⇒ the system trust store | PEM CA-bundle to verify the peer against. |
| `insecure` | `VALUE` u8 (flag) | DIAL | `0` | **DEV ONLY.** Skips server-certificate validation. |
| `path` | `NAME` utf-8 | DIAL | `/` | The extended CONNECT `:path` — which resource the WebTransport session is opened on (`new WebTransport("https://host:port/here")`). Empty is normalised to `/`. **This is an HTTP URL path, not a libtracer graph path**, and it is *not* the `can` kind's `path` key (an advertised group path): kind-private namespaces do not collide, but the two spellings are identical, so read the section heading before copying a row. The LISTEN side of this kind serves every path — it validates `:method`/`:protocol` only — so the key matters when dialing someone else's server (#1023). |

<!-- config-keys:end -->

`tools/gen-dev-cert.sh` emits a self-signed pair for the LISTEN side.

A dial to the wrong resource is not a distinguishable failure: the server refuses
the CONNECT, the session never establishes, and creation answers `TRANSPORT_DOWN` — the
same status a certificate rejection gives. Before [#1023] there was no key at all
and the factory hard-coded `/`, so a SPEC could reach only a root-served session
and any other server needed the direct constructor plus `provide_link`. On the
LISTEN side, `webtransport_transport_t::session_path()` reports the `:path` the
accepted CONNECT named — an observation, never an admission decision.

## Certificate trust on a SPEC-created dialer

Four points, in the order an integrator meets them.

**The default is verify.** A `quic` or `webtransport` dialer created from a SPEC
with *neither* trust key validates the peer's certificate against the system trust
store, and a certificate that does not chain to it is refused: the handshake fails
and creation answers `TRANSPORT_DOWN`. Anything dialing a self-signed peer must say so,
with `ca` or with `insecure = 1`. This is a change of behaviour, not a restatement
of one: before [#918] the DIAL branch hard-coded no-verify and returned *before*
the kind-private parse ran at all, so every SPEC-created dialer skipped validation
and no config key existed that could change it. `core/tests/quic_test.cpp` and
`core/tests/webtransport_test.cpp` drive all five legs — no key, `insecure = 1`,
`ca` = the peer's own cert, `insecure = 0`, and an unrelated `ca` bundle that is
genuinely consulted and still refuses.

**`insecure` wins when both are set.** The credential is built with a single
`if (insecure) … else if (!ca.empty()) …`, so `insecure = 1` together with a `ca`
path is a no-verify dial and the bundle is not consulted. That is deliberate: it
matches the `quic_dial_tls_t` contract the direct-construction path already had, and
it errs toward the mode the operator wrote down explicitly rather than toward a
silently half-applied one.

**Malformed `insecure` fails secure.** Every way of getting the key wrong resolves
to *verify*, because the reader returns "absent" and the field keeps its `false`
default: the key omitted, the value sent as a `NAME` instead of a `VALUE`, an empty
`VALUE` payload, and — because a `VALUE` is little-endian — a wide payload whose
**first** byte is zero, which is what a big-endian `1` looks like on the wire. There
is no spelling of a broken `insecure` that turns validation off.

**`insecure = 0` is not a weaker opt-out.** It is the explicit spelling of the
default, and it verifies.

## Why these are not `conn_settings_t` fields

`conn_settings_t` carries only the keys **every** transport kind shares. That
leanness is a ruling, not an accident (ADR-0043 §5): a kind's private configuration
is parsed by that kind's own factory, inside its own module.

The reason is the module boundary. `quic` lives in a separate link target; a device
that does not link it contains zero QUIC schema, no msquic reference and no feature
macro. Putting `cert`/`key`/`ca`/`insecure` on the shared record would put TLS
vocabulary into the connection settings of a 16 KB MCU that will never speak TLS —
and it would grow once per kind, forever, for keys no other kind can use.

So the answer to "where do I add my kind's new key?" is: **in your factory**, read
out of the raw config TLV it already receives, and in a block on this page. Not on
`conn_settings_t`.

## Pitfalls

- **A typo is silent.** No status distinguishes "key absent" from "key misspelled"
  from "key sent with the wrong TLV type" — all three take the default. The
  connection comes up looking healthy.
- **`can` ignores the universal keys.** Setting `port` or `max_frame` on a `can`
  connection changes nothing; its identity is `ifname` + `node`.
- **`keepalive` has no consumer at all.** It is parsed into `conn_settings_t` and no
  transport in the tree reads the field.
- **`backoff` and `connect_timeout` are dormant.** They parse, they land in
  `conn_settings_t`, and nothing reads them yet.
- **`max_frame` does not only tighten *on the four framed kinds*.** Each of `tcp`,
  `quic`, `webtransport` and `ws` REPLACES the 16 MiB default with whatever non-zero
  value it is given, and the only other bound is the injected backend's
  `max_segment_size()` — `SIZE_MAX` on the default heap backend. So on a host,
  `max_frame = 32 MiB` accepts a 20 MiB frame the default tears down as malformed.
  Treat it as an ingress bound you can loosen, not just clamp (#1035). `udp` is the
  exception, and not by policy: a datagram cannot exceed 64 KiB, so a value above that
  is simply inert there.
- **`peer_named` is off by default**, so a SPEC-created `tcp`/`ws` listener is a
  broadcast link and one request over it draws one reply *per peer*.
- **There is no public builder for this grammar yet.** Every emitter hand-writes
  the pairs ([#902]), so a key's spelling is only as good as the string literal
  next to it.

## How this page is kept true

Every key table above sits inside a marker block naming the source file that reads
it, and `tools/check_config_keys.py` derives the same information from that file —
it finds each `tr::net::config_reader_t` construction and reads the accessor calls
on it, so the key *and* its wire value type come from the code, not from a
maintainer's memory. The gate fails on three things: a key the source reads and the
page omits, a key the page lists and the source no longer reads, and a source file
that reads connection config with no block on this page at all. The last one is
what keeps the sweep honest — a new kind cannot be added with its keys documented
nowhere, and `udp`'s "no kind-private keys" is a derived fact rather than a claim.

The scope of that gate is the `config_reader_t` family, and the scope is deliberate:
it does not see the creation-SPEC envelope, the SUBSCRIBER QoS parse or the ACL
walk, which read the same grammar through their own code at a layer where `tr::net`
is not available.

```
tools/check_config_keys.py            # the gate
tools/check_config_keys.py --list     # the derived inventory, one key per line
```

[#902]: https://github.com/avatarsd-llc/libtracer/issues/902
[#918]: https://github.com/avatarsd-llc/libtracer/issues/918
[#1023]: https://github.com/avatarsd-llc/libtracer/issues/1023
