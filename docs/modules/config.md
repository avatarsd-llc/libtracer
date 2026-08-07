# config — the build's named traits type

```{admonition} In one paragraph
:class: tip
Every compile-time knob a libtracer build has is a member of **one named type**,
`tr::graph::default_config_t` — sizes, padding widths, and the two *policies* the
build selects (which ACL evaluator, which last-known-value slot). An application
picks its configuration by making `tr::graph::config_t` name its own traits type,
once, app-wide; every loose spelling in the library is derived from that alias.
A second reader, `tr::net::config_reader_t`, is the runtime counterpart: typed
accessors over a `config` SETTINGS TLV that arrived from a peer.
```

## What it does

Two different things are called "configuration" here, and keeping them apart is
the point.

**Build configuration** is `default_config_t`. It is not a set of macros and not
a template parameter: it is a struct whose members are `static constexpr` values
and nested type aliases, bound once by an alias. Being one named entity is what
makes a configuration diffable, passable to a test, and assertable on — a scatter
of independent `#define`s can only be read one at a time. It is bound per build
rather than threaded through the API because threading it through produces
byte-identical machine code while forking the process-global stripe and hazard
tables, which costs exactly the RAM the configuration exists to save.

The knob-by-knob narrative — what each knob costs on which target, and which ones
are optimization rather than correctness — is
[the configuration space](../design/config/00-configuration-space.md). This page
is the API surface.

**Runtime configuration** is `config_reader_t`. A connection's settings arrive as
a SPEC `config` SETTINGS TLV: positional NAME-key / value pairs, string values as
`NAME` children and integers as `VALUE` children. All six transport-side consumers
used to hand-roll the same walk — the universal keys plus the tcp, ws, can, quic and
webtransport factories — and this class is their one home. Unknown keys are ignored so
a newer peer can send more than a receiver understands, a key whose value child
has the wrong type is ignored, and a repeated key resolves to its last
well-formed occurrence. Each factory still reads only *its own* keys — what is
shared is the walk, not the vocabulary.

The walk is **pair-consuming**: it advances a whole `(key, value)` pair at a
time, so an unknown key is skipped *together with its value*. That is what lets
the tolerance coexist with positional pairing. Scanning every offset instead made
the grammar ambiguous — a pair whose string value textually equalled a known key
(`link_hint = "addr"`) had that value re-read as a key, binding the following
child as `addr` and, under last-wins, destroying a legitimate earlier one. A
child that is not a `NAME` where a key belongs desynchronizes the stream and the
walk stops there rather than guessing a resync.

`config_reader_t` is the one home for the *transport* config walk, not for every
pair walk in the tree — the scope of that claim is deliberate. `graph_t::create_child`
(the creation SPEC) and the SUBSCRIBER QoS `SETTINGS` parse read the same positional
grammar at L4, where `tr::net` cannot be a dependency, so they carry the same
pair-consuming *rule* without sharing the type. `graph::parse_acl` is the one walk
still scanning every offset; [#906](https://github.com/avatarsd-llc/libtracer/issues/906)
rewrites it whole under the opposite unknown-key ruling and owns that fix.

## Declaring your own build configuration

Inherit, override what differs, and bind the alias in a header your build puts
ahead of `libtracer/config.hpp` on the include path:

```cpp
struct my_node_config_t : tr::graph::default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;   // single-core: no false sharing
    using lkv_slot_t = tr::graph::hazard_slot_t;        // many-core: no shared pointer lock
};
using config_t = my_node_config_t;
```

Inheriting rather than copying means a knob added later inherits its new default
instead of failing to compile.

## The two policies

A policy is a *type* the configuration selects, not a runtime flag, so the branch
it removes is not on the hot path at all.

**ACL policy** — `acl_policy_t` picks how an access decision is evaluated.
`allow_only_policy_t` is the MCU profile: ALLOW entries only, no ordering
question to answer. `full_acl_policy_t` is the host profile: ordered
first-match-per-bit, DENY included. See [security & ACL](security-acl.md) for
what the entries mean.

**LKV slot policy** — `lkv_slot_t` picks how a vertex publishes and reads its
last-known value. `sp_atomic_slot_t` is the default: an
`std::atomic<std::shared_ptr<const rope_t>>` whose reclamation *is* the refcount,
so there is no scheme to implement and no registry to size. `hazard_slot_t` is
the many-core alternative: a lock-free `atomic<node_t*>` reclaimed with hazard
pointers, which removes the pointer-lock both `load` and `store` take in the
default slot. The trade is explicit — the hazard slot buys nothing at one thread,
costs a fixed registry sized by `kHazardReaderSlots`, and has a publish that can
fail under memory exhaustion, which the default slot cannot. Both return an
*owning* handle from `load()`; that is the contract a third policy would have to
satisfy.

## Pitfalls

- **Bind the alias, do not edit the struct.** Overriding `config_t` is a
  supported target-configuration change; editing `default_config_t` in place puts
  your build on a fork of the library.
- **`kCacheLineBytes = 0` is a legal value.** It means "this target has no second
  core to false-share with", and it is an optimization knob in both directions —
  never a correctness one.
- **A `config_reader_t` borrows.** The reader and every `std::string_view` it
  returns point into the decoded TLV's storage; both die with the `tlv_t`.
- **Ignoring an unknown key is deliberate.** A reader that rejects settings it
  does not recognize breaks forward compatibility with a newer peer. This is the
  *opposite* ruling from `parse_acl`, which rejects an unknown key: an ACL is a
  security document, where silently dropping an attribute widens access. The
  tolerance is safe here only because the skip takes the whole pair.

## API reference

```{doxygenstruct} tr::graph::default_config_t
:project: libtracer
:members:
```

```{doxygentypedef} tr::graph::config_t
:project: libtracer
```

```{doxygenvariable} tr::graph::kPinNever
:project: libtracer
```

```{doxygenvariable} tr::graph::kVertexLockStripes
:project: libtracer
```

```{doxygenvariable} tr::graph::kCacheLineBytes
:project: libtracer
```

```{doxygenvariable} tr::graph::kHazardReaderSlots
:project: libtracer
```

```{doxygenvariable} tr::graph::kPinPayloadRatio
:project: libtracer
```

```{doxygentypedef} tr::graph::acl_policy_t
:project: libtracer
```

```{doxygentypedef} tr::graph::lkv_slot_t
:project: libtracer
```

### The selectable policies

```{doxygenstruct} tr::graph::allow_only_policy_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::full_acl_policy_t
:project: libtracer
:members:
```

```{doxygenclass} tr::graph::sp_atomic_slot_t
:project: libtracer
:members:
```

```{doxygenclass} tr::graph::hazard_slot_t
:project: libtracer
:members:
```

### The runtime settings reader

```{doxygenclass} tr::net::config_reader_t
:project: libtracer
:members:
```

See: [the configuration space](../design/config/00-configuration-space.md) (what
each knob costs), [security & ACL](security-acl.md),
[graph](graph.md), [fwd-router](fwd-router.md) (which consumes the settings
reader).
