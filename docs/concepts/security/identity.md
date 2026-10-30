# Subjects and identity

## In one paragraph

An ACL compares subjects, so every remote frame must be turned into one. The application does
that through one hook, `graph_hooks_t::subject_lookup`: given the caller context the frame
arrived with, it writes the subject token and the application's `now`, or answers an error,
which refuses the operation. The caller context of a remote frame comes from its link, and a
session that proved an identity, such as a WebSocket session whose authentication check bound a
subject, hands that identity on. Beside this, a node may publish its public key as the
`:identity` record. Anyone may read it, before any grant, so a peer can pin the key on first
contact and recognise the node later. The record is a claim the library stores and serves, not
a proof.

## The picture

```{mermaid}
:caption: "Legend: vermillion = peer, purple = application, sky blue = record, green = vertex."
flowchart LR
    accTitle: From inbound link to subject token, and the identity a peer pins
    accDescr: A frame from a peer arrives over a session whose authentication check may have bound a subject. The application's subject lookup turns the caller context into a subject token and its own now, which the vertex ACL is evaluated against. Separately, the peer reads the node's identity record, which is served before any grant, and pins its key on first contact.
    P[[peer]]:::peer ==>|"session, maybe authenticated"| C["caller context"]
    C --> L{"subject_lookup"}
    A([application]):::app -->|"installs"| L
    L -->|"token, now"| V("vertex ACL gate"):::vertex
    P -.->|"read :identity, no grant needed"| I["kind, key"]:::record
    I -.->|"pin on first contact"| P
```

## When you need it

When you turn enforcement on, which is the moment you install the lookup. And when peers must
tell the same node reached two ways from two nodes, or notice that a node they met before now
presents a different key.

## How it works

**Local calls are trusted.** The application's own calls carry the empty caller context, and
the gate admits them before the lookup is asked. A remote frame always carries a non-empty
context, its inbound link, so it can never reach the trusted arm.

**The lookup.** `subject_lookup_fn_t` takes the caller context and writes the token into a byte
buffer and the time into `now_ns`. Its error return is a refusal, never a fall back: a caller
that cannot be named fails every gate. A token spelling `EVERYONE@` is refused too. `now_ns`
starts as "unknown", and the gate reads it only when a lease is in play.

**Where a session's subject comes from.** A link mints a peer handle for each accepted session,
and the terminus asks the transport for that session's subject. A session that bound no subject
answers with the transport's own token, such as a stream listener's `p<slot>~<gen>`. A WebSocket
session whose authentication check accepted a credential answers with the subject that check
bound, until the session closes; a later session in the same slot does not inherit it. A peer
*name* in the graph is a different claim: the transport assigns it at accept, and it attests
nothing about the far end.

**`:identity`.** `graph_t::set_identity(kind, key)` installs the record; kind `0x01` is a raw
ed25519 public key of exactly 32 bytes. Every vertex serves the same bytes, so the record is a key
across paths. It resolves above the `READ` gate, because a peer must obtain the key before it can
be granted anything. A node without a key answers `SCHEMA_NOT_FOUND`.

**Trust on first use.** The peer stores the key the first time it reads it and compares it on
every later contact. The library performs no match of its own; the pin and the comparison are the
client's.

## Worked example

What a caller the ACL refuses gets back: an `ERROR` holding one registered status. The reply
does not echo the name it was refused on, so it discloses nothing about what exists.

```{hex-walk} acl/denied-caller-undeclared-app-field
:caption: The refusal, the same for every name a denied caller tries.
u16: the status, 0x0050, tr::access::denied
```

## Cost

### Wire bytes

The subject is never sent. The `:identity` record is 60 bytes for an ed25519 key. A refusal is
the 10-byte `ERROR` above, inside its reply frame.

### Allocations

The lookup writes the token into the `bytes_t` the gate passes it. The identity record is stored
once, at `set_identity`, and each read copies it into the reply.

### Latency

One lookup call per gated remote operation. Installing, clearing and reading `:identity` share
one lock; all three are cold. `identity_test` pins behaviour, not time.

### Flash and RAM per target

| Target | Lookup | Identity |
| --- | --- | --- |
| NARROW | one function pointer and context in the graph's hooks | the stored record, 60 bytes |
| MID | same | same |
| WIDE | same | same |

## What the library does not do

- It does not verify that a node holds the key its `:identity` claims.
- It does not pin a key or compare one for the peer.
- It does not derive a subject from a peer's name in the graph.
- It does not ask the lookup for the application's own calls.

## Pitfalls

- **Symptom:** every remote operation is refused after the lookup is installed. **Cause:** the
  lookup answers its error arm for callers it does not know. **Fix:** map each inbound link or
  session to a token, and grant those tokens.
- **Symptom:** a reconnecting browser keeps its old rights without authenticating. **Cause:** the
  application keyed rights on the peer name, not the subject. **Fix:** key the ACL on the subject
  the authentication check binds.
- **Symptom:** a node's `:identity` changes and peers accept it. **Cause:** the peers do not keep
  a pin. **Fix:** store the key on first contact and compare it after.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates the identity
record in [reference 05 §The node-identity record](../../reference/05-protocol-tlvs.md#the-node-identity-record--identity)
and the subject rules of [reference 05 §0x0A ACL](../../reference/05-protocol-tlvs.md#0x0a--acl).
How a session authenticates is not normative.

**Exercised by:** `denied-caller-undeclared-app-field`, `identity_test`, `acl_test`.

## History

:::{dropdown} How subjects and identity got here
- [ADR-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0018-access-control-authorization-pluggable-subject-token.md)
  made the subject a pluggable token behind one seam.
- [RFC-0011](../../spec/rfcs/0011-node-identity-facet.md) added the `:identity` record, readable
  before any grant.
- [ADR-0086](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0086-identity-is-app-level-key-plus-opaque-credential-anchored-names-are-a-policy.md)
  (2026-10-07) kept trust on first use as the default and made identity the application's.
:::

Next concept: [Noise links](noise-link.md). Go deeper:
[reference 16, WebSocket session authentication](../../reference/16-websocket-session-auth.md#the-subject),
then [reference 07 §the identity record](../../reference/07-host-embedding.md), then the
[subject resolver example](../../examples/acl-subject-resolver.md).
