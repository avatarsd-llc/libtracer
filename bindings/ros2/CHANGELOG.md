# Changelog — `rmw_tracer` (ROS 2 binding)

All notable changes to the **public API** of the `rmw_tracer` ament package are
recorded here, per [CONTRIBUTING](../../.github/CONTRIBUTING.md) / [CLAUDE.md](../../CLAUDE.md).

**This changelog starts at `[0.13.0]`.** Earlier libtracer releases carried no
entries for this package; its history before that is in git and in
[ADR-0023](../../docs/adr/0023-ros2-binding-via-rmw-tracer.md).

**The package is not published.** It is the one binding
[`tools/sync-version.py`](../../tools/sync-version.py) deliberately does not
stamp, and its `package.xml` intentionally reads `0.0.0`: `rmw_tracer` ships to
no registry and is build-verified only (`tools/build-ros.sh`, `ros:jazzy`). The
version headings below track the libtracer release train that the entries were
cut with, not a released ament package version.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.15.0] — 2026-08-23

### Changed

- **The QoS `depth` mapping now has a second axis, and it is the one `rmw_tracer` will have
  to size: BYTES** (core [#1461](https://github.com/avatarsd-llc/libtracer/issues/1461) /
  [#1462](https://github.com/avatarsd-llc/libtracer/issues/1462), RFC-0025 §4.6.1
  Amendment 2). The README's corrected `history` + `depth` row still holds — a
  subscription's target vertex is a STREAM on *this* node and `rmw_create_subscription`
  sizes the ring `rmw_take` pops — but the producer no longer runs any ring machinery at
  all, and the receiver's ring is additionally bounded in bytes by that vertex's own
  injected `mem::block_source_t` (`graph_t::set_ring_source`, or the graph-level default).
  So the entry count is the retention *intent* and the source is the *bound*; a
  `rmw_tracer` subscription that declares `depth = N` and injects nothing gets the
  graph's default heap source. `rmw_get_subscriptions_info_by_topic` reporting a remote
  publisher's depth stays open and is now narrower still: there is no producer ring to
  report.

  **Consequence for a future `rmw_take` implementation:** cross-writer total order is no
  longer implied by ring append order. Order across producers is
  [ADR-0019](../../docs/adr/0019-per-producer-monotonic-origin-timestamp.md)'s
  per-producer HLC stamp, read off the value.

- **`graph_t::propagate` is `[[nodiscard]] result_t<void>`** (core RFC-0008 Amendment 2,
  [#1506](https://github.com/avatarsd-llc/libtracer/issues/1506)). No `rmw_tracer` TU calls
  it today — `src/rmw_tracer/identity.c` remains the package's only real translation unit —
  but the staged `rmw_publish` path in the README's checklist must check the result.

### Documentation

- **The README's nine `file:line` citations into `core/` are re-pinned** to where those
  lines moved across the 0.15.0 core work. Text unchanged; the claims they support are
  the same claims.

## [0.14.0] — 2026-08-21

No API change. `src/rmw_tracer/identity.c` is still the package's only real
translation unit and is untouched; the remaining `rmw_*` entry points remain the
staged work described in the [README](README.md) and
[ADR-0023](../../docs/adr/0023-ros2-binding-via-rmw-tracer.md).

### Documentation

- **The QoS mapping's `depth` row was wrong, and is corrected** ([README](README.md),
  "QoS: there is no `settings` to map onto"). It said ring depth "is not the
  subscriber's to choose". A subscriber **does** get its own queue: a subscription's
  target vertex lives on *this* node, is a STREAM, and `rmw_create_subscription` sizes
  that ring locally with `graph_t::set_history_depth` — it is the ring `rmw_take` pops.
  ROS's per-subscription `depth` therefore maps directly. What has no wire surface, and
  so cannot be set or read remotely, is the **producer's** depth (RFC-0022 §3.C):
  depth is *declared* at each end, never *negotiated* between them. The open question
  is consequently narrower than stated — not "how does a subscriber get a depth" but
  "what should `rmw_get_subscriptions_info_by_topic` report about a remote publisher's
  depth", and there is no basis for a DDS-style QoS-compatibility check either way.
- Citation pins re-anchored against `main` after the RFC-0014 S4 rebase and the
  dispatch cold-half work; the `delivery_policy` / `delivery_mode_t` carrier rows are
  otherwise unchanged.

## [0.13.0] — 2026-08-16

No API change. `src/rmw_tracer/identity.c` — still the package's only real
translation unit, supplying `rmw_get_implementation_identifier` and
`rmw_get_serialization_format` — is untouched; the remaining `rmw_*` entry
points are the staged work described in the [README](README.md) and ADR-0023.

### Documentation

- **The QoS mapping's `delivery_policy` carrier is now cited from
  `core/include/libtracer/subscriber.hpp`, not `vertex.hpp`**
  ([#868](https://github.com/avatarsd-llc/libtracer/issues/868)). Core split
  `vertex.hpp` along its section seams and gave the ACE records their own
  header, so the subscription's packed 16-bit `delivery_policy` — the carrier
  `qos.c` will set at `rmw_create_subscription` time for `reliability` and
  `durability` — moved to the new `subscriber.hpp`. The mapping itself is
  unchanged; only where an implementer reads it from is.
- **Every source citation in [`README.md`](README.md) was re-pinned to the line
  ranges core's v0.13.0 edits moved them to** — the STREAM-ring / owner-side
  `set_history_depth` pin (`graph.hpp`), the RFC-0022 `:settings` write-refusal
  and read-container pins (`graph.cpp`), and the `delivery_mode_t` /
  `set_delivery_mode` pins. Mechanical re-pins by the `doc-citations` gate; no
  claim in the document changed.

### Notes for implementers (from `core`)

These land in `core` at v0.13.0 and shape the work ahead in this package; they
break nothing here, because nothing here consumes them yet. See
[core/CHANGELOG.md](../../core/CHANGELOG.md) for the entries themselves.

- **A `PATH` (`0x06`) body is packed `[u8 len][utf8]` segment records**
  ([RFC-0018](../../docs/spec/rfcs/0018-packed-path-segments.md),
  [#680](https://github.com/avatarsd-llc/libtracer/issues/680)) — wire-breaking.
  A ROS topic name maps to a vertex path, so the topic↔path encoding the
  publish/subscribe TUs will emit is the packed one; `/sensor/temp` is 12 bytes
  rather than 18.
- **A HANDLER's `on_write` takes the writer's `graph::write_ctx_t` as a second
  argument** ([#375](https://github.com/avatarsd-llc/libtracer/issues/375)). The
  service/client TUs (`…/_request` + `…/_response`) are the ones that will
  register handlers, and they must adopt the two-argument signature.
