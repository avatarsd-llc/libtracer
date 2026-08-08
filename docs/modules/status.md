# status & errors — the cross-cutting result taxonomy

```{admonition} In one paragraph
:class: tip
Two closed sets, one per direction. **`tr::graph::status_t`** is what a *local*
call answers with: every `read` / `write` / `await` and every field operation
returns `tr::graph::result_t<T>`, which is `std::expected<T, status_t>`.
**`tr::wire::err_t`** is what a *peer* is told: a registered u16 code carried in
the ERROR TLV, plus registry-side severity and disposition that never travel on
the wire. Neither set is extensible — an application's own failures are
application data, not protocol errors.
```

## What it does

Every other module on this site depends on this one, and nothing here depends on
anything else: the [interface map](interface-map.md) draws `status` as the sink
of the dependency graph. That is the whole point of keeping it separate — the
codec, the graph and the routing plane can all name a failure without agreeing on
anything else.

The two taxonomies answer different questions:

- **`status_t` answers a caller.** It is the L4 control-surface code: the path did
  not resolve, the payload's type does not fit the vertex, the queue is full, the
  await deadline expired, the ACL refused. It is returned by value, never thrown,
  which is what lets the whole runtime build with `-fno-exceptions`.
- **`err_t` answers a peer.** It is the frozen `tr::<concept>::<error>` registry:
  a stable u16 that identifies the failure across implementations and languages,
  so a node written in Rust and a node written in C++ report a truncated frame
  with the same bytes. `err_severity_t` and `err_disposition_t` are registry-side
  metadata — how bad, and whether a retry could help — and are deliberately *not*
  on the wire, so a receiver's policy can differ without a protocol change.

Both sets are **closed**. An application that wants to report "the sensor is
unplugged" writes that into a vertex; it does not mint a protocol error. The wire
codes and their meanings are specified in
[reference §protocol TLVs](../reference/05-protocol-tlvs.md) §error codes; this
page is the C++ surface for them.

## Interface

A local call, a peer-visible failure, and the conversion between the two:

```cpp
namespace tr::graph {
enum class status_t { NOT_FOUND, INVALID_PATH, TYPE_MISMATCH, /* … */ };
template <class T> using result_t = std::expected<T, status_t>;
constexpr const char* to_string(status_t) noexcept;
}

namespace tr::wire {
enum class err_t : std::uint16_t { FRAME_TRUNCATED = 0x0001, /* … */ };
constexpr std::string_view    err_path(err_t) noexcept;        // "tr::frame::truncated"
constexpr err_severity_t      err_severity(err_t) noexcept;
constexpr err_disposition_t   err_disposition(err_t) noexcept;
}
```

`err_path` is the bridge to the human-readable registry name — the same
`tr::<concept>::<error>` string the reference suite and the Wireshark dissector
use, so a log line and a spec section can be matched by eye.

## Checksums

The frame trailer's integrity codes live beside the taxonomy because they are the
other thing every layer needs and no layer owns: CRC-32C (Castagnoli) for the
32-bit trailer and CRC-16-CCITT for the narrow one. Both are incremental — a
state object fed span by span — because a rope is decoded link by link and the
CRC must cover bytes that are never contiguous in memory. Which one a frame
carries is an `opt`-bit decision described in
[frame-codec](frame-codec.md) and specified in
[reference §data format](../reference/01-data-format.md).

## Pitfalls

- **`status_t` is not `err_t` renamed.** They overlap in meaning, not in
  membership: `BACKPRESSURE` is a local answer, `FLOW_BACKPRESSURE` is a wire
  code, and a forwarder that maps one to the other does so explicitly. Do not
  cast between them.
- **Severity and disposition are receiver policy, not protocol.** They are
  compiled into the registry so every node agrees on a *default* reading; they
  are not transmitted, so a node is free to treat a `TRANSIENT` code as fatal.
- **`result_t<void>` is the success-only shape.** A call that returns nothing on
  success still returns a `result_t` — discarding it discards the failure.
- **A link that did not come up is `TRANSPORT_DOWN`, not `NOT_FOUND`.** The two
  read alike locally and diverge on the wire: `NOT_FOUND` becomes
  `tr::path::not_found`, whose registry disposition is PERMANENT, while
  `tr::transport::down` is TRANSIENT. Spending `NOT_FOUND` on a refused dial or a
  listener that could not bind therefore tells a correct peer to stop retrying a
  link that would have come back — which is what the built-in transport factories
  did until [#929](https://github.com/avatarsd-llc/libtracer/issues/929).

## API reference

```{doxygenenum} tr::graph::status_t
:project: libtracer
```

```{doxygentypedef} tr::graph::result_t
:project: libtracer
```

```{doxygenfunction} tr::graph::to_string(status_t)
:project: libtracer
```

```{doxygenenum} tr::wire::err_t
:project: libtracer
```

```{doxygenenum} tr::wire::err_severity_t
:project: libtracer
```

```{doxygenenum} tr::wire::err_disposition_t
:project: libtracer
```

```{doxygenfunction} tr::wire::err_path
:project: libtracer
```

```{doxygenfunction} tr::wire::err_severity
:project: libtracer
```

```{doxygenfunction} tr::wire::err_disposition
:project: libtracer
```

```{doxygenstruct} tr::crc::crc32c_state
:project: libtracer
:members:
```

```{doxygenstruct} tr::crc::crc16_ccitt_state
:project: libtracer
:members:
```

```{doxygenfunction} tr::crc::crc32c(std::span<const std::byte>)
:project: libtracer
```

```{doxygenfunction} tr::crc::crc16_ccitt(std::span<const std::byte>)
:project: libtracer
```

See: [frame-codec](frame-codec.md) (where the codes are produced),
[graph](graph.md) (where `result_t` is returned),
[interface map](interface-map.md),
[reference §protocol TLVs](../reference/05-protocol-tlvs.md).
