# 16 — WebSocket session authentication

*Descriptive reference. This section describes SESSION behaviour on the WebSocket transport
— who a node will talk to, and when. It is not part of the normative wire protocol: nothing
here changes a TLV, a frame layout or a codec, and an implementation that never
authenticates is still conformant. The normative surface is [`../spec/v1.md`](../spec/v1.md).*

## The problem: a browser cannot present a header

A libtracer node reachable over WebSocket has two natural places to decide whether it will
serve a peer, and until now both were **before the HTTP 101**:

- a **token header** on the opening GET, which is what a native dialer (board-to-board,
  a CLI, a gateway) sends;
- a **cookie**, which is what a browser can be made to send — but only if some *other*
  HTTP endpoint minted it first.

The browser `WebSocket` API **cannot set request headers**. There is no argument for them
and no interception point. So a browser client cannot present a token at the handshake at
all, which leaves an embedder two options, both bad:

1. keep an HTTP login endpoint alive purely to mint a cookie — a second auth surface, a
   second session store, and a CORS/SameSite problem, on a node whose whole point was that
   the graph *is* the API;
2. put the credential in the WebSocket URL's query string — the classic workaround, and a
   credential leak: URLs land in server logs, in browser history, in `Referer`-adjacent
   surfaces, and in any proxy in between. **libtracer rejects this option**; a node should
   not be documented into leaking its own credentials.

## The answer: an in-band authentication frame

A third admission point, **after** the 101 and **before** the session is served:

> The socket is upgraded. It exists. It is served **nothing** until it presents a credential
> the node accepts, and it is **closed** if it does not present one in time.

The credential travels as an ordinary WebSocket data frame, which a browser *can* send. The
handshake-header path is unchanged and still available to peers that can use it; the two are
complementary, not alternatives, and a node may require both.

### One node, two kinds of peer

A node that is reachable by a browser is usually also reachable by a **native peer** — another
node, a CLI, a gateway — and a native peer authenticates at the handshake, because it can. So
the interesting configuration is not "frame instead of header"; it is **both at once, on the
same endpoint**, with each peer using the one it is capable of.

That only works if the handshake check can say *"this one is already authenticated"*. Otherwise
installing the frame check makes it mandatory for everybody, and a peer whose whole credential
was in the header — and which has no way to send a frame — is closed at the deadline with a
perfectly good credential. So the handshake check answers **three** ways rather than two:

| Handshake verdict | Then |
| --- | --- |
| refuse | no upgrade; the peer never reaches a session |
| admit | upgraded, and **not** authenticated: a credential frame is required, and the deadline runs |
| admit, authenticated | upgraded and served from the first frame; no credential frame, no deadline |

The third verdict is what makes the two admission points composable rather than exclusive. It
is a property of one **session**, not a switch on the link: the same node answers "admit,
authenticated" to a peer presenting a header and "admit" to a browser presenting nothing, in
the same second.

A node that serves only browsers never needs it, and a node with no frame check is unaffected
by it.

### What "served nothing" means

Between the 101 and acceptance, the session is not a peer. Concretely, an unauthenticated
session:

| It cannot… | because |
| --- | --- |
| read or write a vertex | its frames go to the credential check, never to the graph |
| subscribe | same — a SUBSCRIBE is a graph op, and no graph op is reachable |
| receive a subscription push | a broadcast skips it |
| receive a directed reply | it cannot be resolved to a sending endpoint |
| be discovered | it is absent from the peer census, so it appears in no synthesized `:children[]` |

That list is deliberately the *whole* surface, inbound and outbound. Gating only the inbound
direction would leave a socket that presented nothing still receiving every value the node
publishes — which is the more serious of the two leaks.

### The payload is opaque

The node does **not** interpret the credential. The frame is a **carrier**: its payload is
handed to the embedder's check verbatim, and what counts as a valid credential is entirely
that check's business.

This is a load-bearing commitment rather than an omission. A bearer token is simply the
first payload kind; the same frame carries a challenge–response exchange, and is intended to
carry an ed25519/Noise handshake unchanged when that lands. An authentication frame
specified as "a token, and here is its layout" would have had to be replaced; one specified
as a carrier does not.

Accordingly the check answers one of three verdicts, not two:

- **accept** — serve this session from the next frame on;
- **continue** — not finished; expect another frame (a multi-round-trip handshake);
- **reject** — close it.

Any verdict may carry a **reply payload** back to the peer in the same shape — the responder
message a handshake needs, or an application-level explanation alongside a rejection.

### The subject

On acceptance the check may bind a **subject** to the session: the identity of whoever is on
the other end, as text (an identity that is natively bytes — a public key — is spelled in
hex or base64).

The session *carries* the subject and publishes it for operators. The **handler-side half**
now exists: `on_write` takes a `write_ctx_t` whose `subject` is the writer's resolved subject
token — the very value the vertex's ACL gate was evaluated against
([#375](https://github.com/avatarsd-llc/libtracer/issues/375) Part 1). What is still open is
the **join**: a session subject bound here does not yet become the graph's caller context for
frames that arrive on that session, so today a handler sees the inbound link's (or bus peer's)
name rather than an authenticated session identity. The authentication frame is the right
place to *capture* an identity because it is the first moment one is known; it is not the
right place to decide the whole authorization model.

## The deadline

An unauthenticated session is a **new** kind of resource holder. Before the frame existed, a
peer that failed admission never reached a session slot at all — it was refused at the
handshake. Now it holds one while it is deciding what to send, which on an embedded node
with a small peer cap is a cheap denial of service: open sockets, say nothing, and the cap
is spent.

So the window is bounded. A session that has not been accepted within a configured deadline
is closed. Two properties matter more than the exact number:

- **The deadline is not extended by the peer's own traffic.** A multi-frame handshake gets
  the whole window for *all* of its frames. A peer that could refresh the deadline by
  sending anything at all would effectively have none.
- **An expired session cannot deny a live peer its slot.** The reap that frees expired
  sessions runs when a new peer arrives, not only on the periodic sweep — so admission
  always answers from sessions that are still live, whatever the sweep's timing.

The deadline bounds *this* exposure and no other. A client that connects a TCP socket and
never upgrades at all is an ordinary HTTP-server concern, governed by the server's own
socket budget, and is neither worsened nor improved by anything here.

## Close codes

A rejected peer and an expired peer need different client behaviour — re-prompt for a
credential versus retry the connection — so they are **different close codes**, in RFC 6455's
application range (4000–4999):

| Code | Meaning | Client should |
| --- | --- | --- |
| `4401` | the credential was refused | not retry with the same credential; obtain a new one (mnemonic: HTTP 401) |
| `4408` | the deadline expired before a credential arrived | retry; the credential itself was never judged (mnemonic: HTTP 408) |
| `4403` | the session was REVOKED by the node (`close_peer`) | stop reconnecting; the credential was fine, the access was withdrawn (mnemonic: HTTP 403) |

The third is not an authentication verdict at all — it is the administrative teardown
`bus_link_t::close_peer` performs, and it is listed here because it shares the wire surface
and a client that cannot tell it from the other two behaves wrongly in the most expensive
direction: a revoked controller reading its close as a network fault reconnects forever.

The code is written to the peer **before** the socket is torn down. A close that shut the
socket first would deliver no code at all, and a client left to infer the reason from a bare
disconnect will infer it wrong — most often by retrying a credential that will never be
accepted.

Both codes are chosen once, here, so that every implementation and every client agrees on
them.

## Observability

Refusals at the three admission stages are counted **separately**, because they are facts
about different actors and summing them hides which is happening:

| Signal | Says |
| --- | --- |
| handshake refusals | a header/predicate check turned a peer away, or the peer cap was full — the peer never reached a session |
| credential rejections | a credential was **offered and refused** — the shape of a brute-force attempt, or of a fleet holding a stale token |
| deadline expiries | peers that connected and **said nothing** — a trickle is normal (a tab closed mid-login); a rate that tracks connection attempts is not |

Unauthenticated sessions are deliberately **not** enumerated per-session. A per-session
census of things that have not identified themselves is a census of an attacker's socket
count; the link-level tallies above are the right altitude.

## Reference implementation

The ESP-IDF WebSocket server link (`httpd_ws_link_t`,
[`integrations/esp-idf/libtracer/`](https://github.com/avatarsd-llc/libtracer/blob/main/integrations/esp-idf/libtracer/httpd_ws_link.cpp))
implements this: `set_auth_cb` installs the check, the constructor takes the deadline, and
the close codes are `kCloseAuthFailed` / `kCloseAuthTimeout`. The three-valued handshake
verdict is `set_admission_verdict_cb` with `admission_verdict_t::ADMIT_AUTHENTICATED`. Its
host suite (`httpd_ws_auth_test`) pins each property above against the real translation unit.

See also [12-deployment-profiles.md](12-deployment-profiles.md) for where a browser-facing
node sits, and [13-network-formation.md](13-network-formation.md) for how peers find each
other once they are allowed to talk.
