# Security

## In one paragraph

libtracer does authorization, not authentication. A node decides what a caller may do from two
things: a subject token, which says who the caller is, and the ACL on the vertex it addresses,
which says what that subject may do there. Proving who the caller is belongs to the link and to
the application. The library offers two such proofs: a WebSocket session can bind the subject its
authentication check accepted, and a Noise link admits only frames sealed by a holder of the
link's pre-shared key. A node also publishes its public key as `:identity`, a claim a peer can pin
on first contact and compare later. Calls made locally, by the node's own application, are
trusted, and enforcement stays off until the application installs the hook that names its
callers.

```{toctree}
:hidden:
:maxdepth: 1

Access control lists <security/acl>
Subjects and identity <security/identity>
Noise links <security/noise-link>
```

## The picture

```{mermaid}
:caption: "Legend: vermillion = peer, blue = frame, sky blue = record, green = vertex."
flowchart LR
    accTitle: Who a frame is from, and what it may do
    accDescr: A peer sends a frame over a link, which may be a Noise link that admits only holders of its pre-shared key. The application's subject lookup turns the inbound link into a subject token. The vertex's effective ACL, its own entries plus inherited ones, grants or refuses the operation's right. A refused operation answers ERROR access denied.
    P[[peer]]:::peer ==>|"Noise or plain link"| F[/"FWD frame"/]:::frame
    F -->|"inbound link"| L{"subject lookup"}
    L -->|"subject, now"| A["effective ACL"]:::record
    A -->|"grants the right"| V("vertex"):::vertex
    A -->|"refuses"| E["ERROR access denied"]
```

## When you need it

As soon as a node takes frames from a peer you do not fully control: a browser, another
organization's gateway, a shared bus. A node that talks only to its own processes over loopback
may leave enforcement off, which is the default.

## How it works

| Piece | What it answers | Page |
| --- | --- | --- |
| ACL | what a subject may do on a vertex and its subtree: `READ`, `WRITE` | [Access control lists](security/acl.md) |
| subject lookup | who sent this frame, as the token the ACL compares | [Subjects and identity](security/identity.md) |
| `:identity` | which key this node claims, readable before any grant | [Subjects and identity](security/identity.md) |
| Noise link | whether the far end holds this link's pre-shared key | [Noise links](security/noise-link.md) |

**Per hop, at the receiver.** Each node checks the frames it receives against its own ACLs. A
frame forwarded on leaves one link's protection and enters the next; nothing here is end to end.

**The owner is local.** Only the node's own application, the empty caller context, writes
`:acl`. No remote subject can grant itself rights.

**Failures are values.** A refused operation answers `ERROR{tr::access::denied}`. A Noise link
answers nothing in clear: every failure there is a silent drop and one counter step.

## Cost

### Wire bytes

None on a plain link: the subject is never sent, and an ACL travels only when the owner writes
or a caller reads `:acl`. A Noise link adds 25 bytes to each frame and a 58- and 50-byte
handshake per session.

### Allocations

With no subject lookup installed, the data path pays one null check. With one, each gated
operation evaluates the vertex's cached effective ACL, rebuilt only after an `:acl` on its path
changes. A Noise link draws its session state once, when it is built.

### Latency

Not benched on this page; each child names its tests and benches.

### Flash and RAM per target

| Target | ACL policy | Noise link |
| --- | --- | --- |
| NARROW | `allow_only_policy_t`, the default | not shipped |
| MID | same | not shipped on ESP-IDF yet |
| WIDE | `allow_only_policy_t`, or `full_acl_policy_t` with DENY | on a host, over UDP |

## What the library does not do

- It does not authenticate callers on a plain link. The transport or the application does.
- It does not protect a frame beyond the hop that received it.
- It does not hold keys, entropy or a clock; the application supplies each.
- It does not let a remote subject write an `:acl`.

## Pitfalls

- **Symptom:** an ACL is written and changes nothing. **Cause:** no subject lookup is installed,
  so enforcement is off. **Fix:** install `graph_hooks_t::subject_lookup`.
- **Symptom:** a remote write is refused that the same code allows locally. **Cause:** local calls
  are trusted and remote ones are gated. **Fix:** grant the remote subject the right it needs.

## Where the rules live

**Normative source:** [v1 §3](../spec/v1.md#3-wire-format), which incorporates the ACL model of
[reference 05 §0x0A ACL](../reference/05-protocol-tlvs.md#0x0a--acl) and, for a link that uses it,
the [Noise link binding](../reference/23-noise-link-binding.md#1-what-the-binding-is-and-where-it-sits).
How a node authenticates a caller on any other link is not normative.

**Exercised by:** `acl-aces`, `denied-caller-undeclared-app-field`, `nnpsk0-appendix-a`,
`acl_test`, `noise_link_test`.

Next concept: [Access control lists](security/acl.md). Go deeper:
[reference 05 §0x0A ACL](../reference/05-protocol-tlvs.md#0x0a--acl), then
[reference 16, WebSocket session authentication](../reference/16-websocket-session-auth.md), then
[reference 23](../reference/23-noise-link-binding.md).
