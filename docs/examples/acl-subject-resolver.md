# The caller context is not the subject (L4 auth / ACL)

An ACE names a **subject token** — opaque bytes. An operation arrives carrying a **caller
context** — this node's own NAME for the inbound link the frame came in on. Those are different
things, and the pluggable `subject_resolver_fn_t` is the seam between them
([ADR-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0018-access-control-authorization-pluggable-subject-token.md)):
it is where an integrator turns *"the frame came in on link `ws:7`"* into *"and that link belongs
to `alice`"*.

That split is what lets the ACL model stay fixed while identity gets stronger. v1's
transport-authenticated peer id and a later raw-key ed25519 identity are both just tokens
([ADR-0045](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md));
libtracer does **authorization**, and the transport does authentication.

## What to notice

- **The ACE names the principal, not the link.** The example's ACL says `alice`; nothing anywhere
  says `ws:7`. Only the resolver — integrator code — knows the two are connected, which is why a
  principal survives being dialled in on a different link tomorrow.
- **The ERROR arm is a deny, not a fallback.** "I cannot name this caller" — a stale link, a
  revoked peer, a lookup that failed — refuses at *every* gate. The predecessor of this signature
  returned `std::optional`, whose `nullopt` meant **fully trusted**, so an unresolvable caller
  used to be granted everything, `WRITE_ACL` included
  ([#905](https://github.com/avatarsd-llc/libtracer/issues/905)). The example's last check is
  that specific disaster: the unnameable caller cannot rewrite the `:acl` to grant itself access.
- **The empty caller context never reaches the resolver.** The example counts invocations through
  its `ctx`: two local writes, zero calls. The trusted local channel is settled *before* the
  resolver, so a resolver author never has to have an opinion about it — and a remote op, which
  always carries a non-empty link NAME, cannot reach that arm.
- **It is a `{fn, ctx}` pair, not a `std::function`.** Assigning a `std::function` destroys the
  old target, so a setter racing a gate freed the resolver's captured state while a reader was
  inside the call. A bare function pointer is publishable in one word, so the pair lives in a
  `sink_slot_t` and the gate reads a coherent snapshot
  ([#1049](https://github.com/avatarsd-llc/libtracer/issues/1049)). Whatever state the resolver
  needs travels in `ctx`, which the caller owns and must keep alive across every gated operation.
- **Install it at wiring time, from one thread, before frames flow.** `configure_subject_resolver`
  is configuration, which the verb says on purpose.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/acl_subject_resolver.cpp
:language: cpp
:linenos:
```

See also: [security-acl module](../modules/security-acl.md) ·
[graph module](../modules/graph.md) ·
[network formation](../reference/13-network-formation.md) ·
[open by default](acl-open-by-default.md) ·
[the reserved wildcard subject](acl-everyone-reserved.md).
