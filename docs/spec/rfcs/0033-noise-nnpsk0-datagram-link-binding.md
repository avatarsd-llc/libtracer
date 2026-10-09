<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0033 — A minimal Noise link binding: NNpsk0 over a datagram carrier

<!-- status: proposed -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0033 |
| **Title** | A minimal Noise link binding: NNpsk0 over a datagram carrier |
| **Status** | **proposed** (2026-10-09). The maintainer approved the direction on 2026-10-09 (issue [#2063](https://github.com/avatarsd-llc/libtracer/issues/2063)). This document turns that direction into normative text. It needs **maintainer approval** before it is accepted, and §15 lists the choices still open, each with a recommendation. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-10-09 |
| **Comment window** | Waived by default while the project is solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"). Invoke it explicitly if outside input is wanted. At drafting, `docs/implementations.md` still lists no registered implementation, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** It adds an optional normative link binding: a datagram layout, a handshake, and MUSTs that a peer can observe on the wire. It also adds nouns to the `:stats.link.<child>` block of [reference/05](../../reference/05-protocol-tlvs.md), a normative annex. GOVERNANCE.md reserves both for an amendment. It changes no existing wire byte: a link that does not use the binding is unaffected. |
| **Tracking issue** | [#2063](https://github.com/avatarsd-llc/libtracer/issues/2063). It is a narrow slice pulled forward from [#1993](https://github.com/avatarsd-llc/libtracer/issues/1993) (an opaque credential member in `:identity` and the "Tracer over Noise" binding, v1.0.0). |
| **Target spec version** | v1 itself. `docs/spec/v1.md` still reads "(DRAFT)". |
| **Builds on** | [ADR-0045](../../adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) Decision 4 (link confidentiality is the link's job, by a Noise channel) and Decision 5 (trust is per hop); [ADR-0086](../../adr/0086-identity-is-app-level-key-plus-opaque-credential-anchored-names-are-a-policy.md) Decision 8 ("Tracer over Noise" is an optional normative link binding); [RFC-0014](0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) (link liveness); [RFC-0010](0010-owner-app-fields-and-schema.md) Amendment 2 (the `:stats.link.<child>` census); [RFC-0029](0029-one-path-primitive.md) §8.2 (a session boundary advances the generation). |
| **Leaves to #1993** | The `:identity` credential member, the static-key patterns (IK, XX, KK) over the ed25519 node key, and the credential in the handshake payload. §7 says how they attach to this binding. |
| **Relates to** | [#1649](https://github.com/avatarsd-llc/libtracer/issues/1649) (an ESP-IDF UDP link with Noise), which should be built against this binding (§10.3). |

> **Numbering note.** Numbering gaps and why they are not reused are recorded in the
> [ADR and RFC index](../../adr-rfc-index.md#numbering-gaps).

---

## 1. Summary

An embedder sometimes needs one authenticated request and reply between two nodes before any
site identity exists, for example a recovery command to a board. Neither node has a key the
other knows. What the application can provision is a shared secret. This RFC specifies the
smallest link binding that carries Tracer frames over that secret:

1. **Pattern.** `Noise_NNpsk0_25519_ChaChaPoly_SHA256`, with a fixed prologue and a 32-byte
   pre-shared key (PSK) that the application supplies. The PSK never appears in a frame or in
   a config record a peer can write or read (§5.2).
2. **Carrier.** Any link that delivers whole datagrams, possibly lost, duplicated or reordered.
   UDP is the first. One datagram carries one Noise message, behind a one-byte type (§5.3).
3. **Frames.** One transport message carries exactly one Tracer frame, unchanged. Each one costs
   25 bytes: a type byte, an explicit 8-byte nonce and the 16-byte tag. Nothing is fragmented,
   and a frame above the bound is refused, never truncated (§5.3).
4. **Nonces, replay, rekey.** Nonces are explicit and never reused. A sliding window refuses
   replays, and only after the tag verifies does it move. A rekey is a new handshake, with a key-phase
   bit so that the two sessions overlap cleanly (§5.5, §5.6).
5. **No library clock.** Handshake retransmit and session expiry run only when the application
   passes `now`. Without it, a handshake or a session ends on its other events (§5.7).
6. **Receiver pays.** All session state is part of the link and comes from the link's own source.
   Before the PSK is proven, the responder spends two key derivations and one tag check on a
   datagram, and no Diffie-Hellman. Nothing is allocated (§5.8).
7. **Failure is silent on the wire and loud in the counters.** A failed handshake or a failed
   decrypt is counted and never answered in clear (§5.9).
8. **Reporting.** A DIAL link reports `up` only while it holds a confirmed session. Eight
   counters join the link's `:stats` block (§5.10, §5.11).

The binding sits below the TLV layer. It changes no frame, no type code and no conformance
vector. The core stays crypto-free (ADR-0045, ADR-0086 §Consequences): the binding is
implemented by the module catalog's `security_noise` slot, over crypto primitives the host
provides.

## 2. Motivation

### 2.1 The first contact has no identity yet

ADR-0045 orders the security roadmap as vertex-dance login, then ed25519 trust on first use,
then Noise on plaintext links. ADR-0086 Decision 8 makes "Tracer over Noise" a normative binding
with the patterns XX or IK over the node's ed25519 key, and #1993 will specify it before the
v1.0 freeze. Both assume that the nodes have static keys and that at least one side knows, or can
pin, the other's.

Some deployments need authentication before that holds:

- a board out of its box, which an operator wants to send one recovery command to over the local
  network;
- a replacement board whose node key is new, talking to a controller that has not enrolled it;
- a bench tool that drives a board through a factory step.

In each case the application can install a shared secret (on a label, in a provisioning image,
in the tool's config) long before any key pinning or enrolment exists. Today such a deployment
has two choices: send the command in clear, or invent its own envelope. The first is the
exposure ADR-0045 §"Honest caveat" names. The second is the interop failure ADR-0086 Decision 8
was written to prevent.

### 2.2 Why NNpsk0

`NNpsk0` is the Noise pattern with no static keys in which the PSK is mixed in before the first
message:

```
NNpsk0:
  -> psk, e
  <- e, ee
```

It has three properties this use case needs, and no other pattern has all three:

- **It needs nothing but the PSK.** No static key, no pinning, no credential.
- **The first message proves the PSK before any public-key work.** The responder checks the
  first message's tag with a key derived from the PSK. A sender without the PSK costs the
  responder two key derivations and one 16-byte tag check, and never a Diffie-Hellman. That is
  what lets the responder pay nothing before the PSK is proven (§5.8).
- **The session is forward secret.** The `ee` exchange means a PSK that leaks later does not
  decrypt a recorded session (§6.3).

`psk2` (the PSK at the end of the second message) or `psk1` would let anyone without the PSK
make the responder generate a key and run a Diffie-Hellman. `NKpsk0` or `KKpsk0` need static
keys, which is #1993's job. §13 lists the other alternatives.

### 2.3 Why a datagram carrier first

UDP is the cheapest link for two boards on one network, and #1649 asks for one. A datagram
binding also has to settle the hard cases first: loss, duplication, reordering and replay. A
stream binding is the easier special case and can follow (§13).

## 3. The rulings this document encodes

**From issue [#2063](https://github.com/avatarsd-llc/libtracer/issues/2063) (direction approved
by the maintainer on 2026-10-09):**

1. A minimal Noise link binding, pulled forward from #1993 as a narrow slice. #1993 keeps the
   `:identity` credential member and the identity-bearing patterns on v1.0.0.
2. Pattern `Noise_NNpsk0_25519_ChaChaPoly_SHA256`, with the PSK supplied by the application
   through the link's config.
3. KK and IK over the ed25519 identities of ADR-0045 are named as later bindings, not specified here.
4. Framing over a datagram carrier (UDP first): what one datagram carries, the size bound, and how
   a frame maps onto Noise transport messages.
5. No library timers or clock reads: handshake retransmit and session expiry are driven by the
   application passing `now`.
6. Session state comes from the link's own source. There are no library-internal buffers, and a
   peer cannot make the node allocate before the PSK is proven.
7. A failed handshake or a failed decrypt is counted and never answered in clear.
8. The link reports up and down, and counters under the link's `:stats`.

**Standing constraints** (`CLAUDE.md` §Design rules): no timers or clock reads in libtracer; no
library-internal buffers; the receiver pays; compile-time policy by default; complexity is lowered
by deleting branches, never by splitting helpers. ADR-0045 adds that the core takes no
cryptographic stance. §9 checks each one.

## 4. Vocabulary

Proposed for `CONTEXT.md` at acceptance, if a glossary line is free (it is at its cap, as
ADR-0086 §Terms notes). Until then the terms are defined here.

- **Noise link.** A link whose frames travel inside a Noise session, over an inner **carrier**
  link. The router sees an ordinary link.
- **Carrier.** The inner link that moves the Noise link's datagrams: UDP here. It preserves
  datagram boundaries and may lose, duplicate or reorder them.
- **Initiator, responder.** The side that sends the first handshake message, and the side that
  answers it. The connection's role fixes them: a DIAL connection is the initiator, a LISTEN
  connection the responder. They never swap.
- **Session.** The two transport keys, the send nonce and the replay window that one completed
  handshake yields.
- **Confirmed session.** A session that has carried at least one valid transport message from the
  initiator. The initiator's session is confirmed when it accepts the second handshake message.
- **Key phase.** One bit that tells a receiver which of its two sessions a transport message was
  sealed under. Each new session takes the other phase.

## 5. The normative change

The key words are those of [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119). "Noise" means
*The Noise Protocol Framework*, revision 34 (2018-07-11). Byte strings are written in hex, and
multi-byte integers are little-endian, as everywhere in Tracer.

### 5.1 What the binding is, and where it sits

**Normative.** A link that carries Tracer frames inside a Noise session with this pattern MUST
follow this section. A link that does not follow it is not a conforming Tracer Noise link
(ADR-0086 Decision 8).

- The binding sits **below the TLV layer**. The frames it carries are byte-identical to the frames
  the same link would carry without it, including any CRC trailer. The router, the codec and every
  conformance vector are unchanged.
- A Noise link **MUST NOT send any Tracer frame in clear**, and MUST NOT fall back to the plain
  carrier when it has no session. A send without a session fails as a send on a down link does
  (RFC-0014 §4: "ops on a down link fail fast").
- The binding is **per hop**. A frame forwarded beyond the far node leaves this session there.
  Nothing here gives end-to-end protection, as ADR-0045 Decision 5 says.

### 5.2 Protocol name, prologue and PSK

**Normative.**

- **Protocol name:** the 36 ASCII bytes `Noise_NNpsk0_25519_ChaChaPoly_SHA256`. They are longer
  than `HASHLEN` (32), so the initial `h` is their SHA-256 (Noise §5.2).
- **Prologue:** the 20 ASCII bytes `Tracer over Noise v1`
  (`547261636572206f766572204e6f697365207631`). Both sides MUST use exactly these bytes. A future
  binding version changes the prologue, so two versions fail the first tag check instead of
  half-working.
- **PSK:** exactly 32 bytes (Noise §9). The application supplies it.
  - It MUST NOT appear in any frame, in any `SPEC` or `SETTINGS` record, or in any `:` field, and no
    read can return it. A `SPEC` that creates a Noise link names the PSK by **reference**, a
    key-store name that the application resolves locally. The key bytes come from a host seam,
    never from the wire. A creating `SPEC` can arrive over a link that is not protected, so a key
    carried in it would be exposed.
  - Each side of a link uses the same PSK. A PSK SHOULD be used by one link pair only (§6.4).
  - The PSK SHOULD be 32 bytes from a cryptographic random source. A PSK derived from a
    human-chosen secret MUST be derived by the application with a deliberately slow key-derivation
    function first (§6.2).
- **Handshake payloads:** the first message's payload MUST be empty. The second message's payload
  is exactly one byte, the **key-phase byte**: bit 0 is the key phase of the new session (§5.6),
  and bits 7–1 MUST be zero. A receiver MUST treat any other payload as a failed handshake. No
  application data rides in a handshake message (§6.5).
- **Crypto primitives:** X25519 (RFC 7748), ChaCha20-Poly1305 (RFC 8439) with Noise's nonce encoding
  (32 zero bits, then the 64-bit counter little-endian), and SHA-256 with Noise's HMAC-based HKDF.
  The **ephemeral keys** MUST come from a cryptographic random source that the application
  injects. The library has no entropy source of its own.

### 5.3 Datagram framing and the size bound

**Normative.** One carrier datagram carries exactly one Noise message, preceded by a one-byte
**type**:

| type | message | layout after the type byte | datagram size |
| ---- | ---- | ---- | ---- |
| `0x01` | handshake, first message (`-> psk, e`) | `e.pub` (32) ‖ tag of the empty payload (16) | exactly **49** B |
| `0x02` | handshake, second message (`<- e, ee`) | `e.pub` (32) ‖ encrypted key-phase byte (1) ‖ tag (16) | exactly **50** B |
| `0x04` | transport message, key phase 0 | `nonce` (u64 LE, 8) ‖ ciphertext (frame length) ‖ tag (16) | 25 B + frame |
| `0x05` | transport message, key phase 1 | as `0x04` | 25 B + frame |

Every other type value is reserved. A receiver MUST drop a datagram with a reserved type, or a
handshake datagram of any other length, and count it in `malformed_rx`. The type byte and the
nonce travel in clear and are not authenticated additional data. Changing either one makes the
tag check fail, so neither can steer a receiver into accepting anything (§6.6).

**A frame maps to exactly one transport message.**

- The plaintext of a transport message is either **empty** or **exactly one complete Tracer
  frame** (header, body and any trailer). An empty plaintext is a **confirmation** (§5.4) and is
  never delivered. A plaintext that does not parse as exactly one frame, with no byte left over,
  MUST be dropped after decryption and counted in `malformed_rx`.
- Several frames are never packed into one message, and one frame is never split across messages.
  Batching is the application's decision (RFC-0025 Amendment 4) and arrives as one BATCH frame.
- The transport ciphertext is a Noise transport message. Its encryption is Noise's
  `EncryptWithAd` with **empty** associated data, under the nonce the datagram carries
  (Noise §11.4, "out-of-order transport messages").

**The size bound.**

- A Noise message is at most 65535 bytes (Noise §3), so a frame is at most **65519** bytes. The
  carrier bounds it further: a UDP datagram is at most 65507 bytes over IPv4, so a frame is at most
  65482 bytes there.
- The link's universal `max_frame` key keeps its meaning on a Noise link: **the largest Tracer
  frame**. The link's datagram bound is `max_frame + 25`, and its receive buffer is sized from
  that.
- A sender MUST refuse a frame above `max_frame`. It is dropped whole and counted in
  `noise_oversize`, never truncated (#1074).
- A receiver MUST drop a datagram above `max_frame + 25` bytes and count it in `malformed_rx`,
  under the same "refuse, never truncate" rule as the plain UDP link.
- A sender SHOULD keep `max_frame + 25` within the path MTU (1200 bytes is a safe default on an
  unknown path), because the binding does not fragment and a lost IP fragment loses the whole
  datagram.
- A carrier whose datagram bound is below 50 bytes cannot carry the second handshake message,
  and a Noise link MUST refuse to be built over it.

### 5.4 The handshake

**Normative.** The procedure follows Noise §5 for the pattern of §2.2, with the tokens processed
as Noise §9 requires for a PSK handshake: `psk` calls `MixKeyAndHash(psk)`, and every `e`
calls `MixKey(e.pub)` after `MixHash(e.pub)`. Appendix A traces every step.

**Initiator (DIAL).**

1. A handshake **attempt** starts when the link is demanded and has no session, or when a rekey is due
   (§5.6). The initiator draws a fresh ephemeral key, writes the first message, sends it, and keeps
   its 49 bytes and the handshake state in the link's handshake slot (§5.8).
2. While no valid second message has arrived, it **retransmits the same 49 bytes** each time the
   application's `now` passes the retry deadline (§5.7). Inside one attempt, every retransmission
   is byte-identical.
3. It MAY end an attempt and start a new one, with a new ephemeral key, when `now` passes the
   attempt deadline. While a standing binding holds the link, it MUST NOT give up for good
   (RFC-0014 §4: no give-up bound and no terminal state).
4. When a second message decrypts, the initiator splits the keys (Noise §5.2: the first key
   encrypts initiator→responder, the second responder→initiator), takes the key phase from the
   payload, and installs the session as **current**. Its earlier session, if it had one, becomes
   **previous** (§5.6).
5. It then MUST at once send a **confirmation**: a transport message with an empty plaintext, on the
   new session. If that is lost, the next transport message the initiator sends confirms the
   session in its place.
6. A second message that fails its tag check is dropped and counted in `noise_handshake_failed`. A
   second message that arrives when no attempt is in flight is dropped and counted in `noise_stale`.
   Neither one is answered, and neither ends the attempt.

**Responder (LISTEN).**

1. On a first message, the responder runs `MixKeyAndHash(psk)`, `MixHash(e.pub)` and
   `MixKey(e.pub)`, then checks the tag. **This is the PSK proof.** If the tag fails, the datagram
   is dropped and counted in `noise_handshake_failed`. Nothing is sent, no state changes, and no
   Diffie-Hellman has run.
2. On a first message that is **byte-identical** to the one behind its pending handshake, the
   responder MUST resend its stored second message unchanged, and count it in
   `noise_retransmits`. Sending a fresh second message would split the two sides onto different
   keys.
3. On a first message whose ephemeral key is the one behind its **current** session, the responder
   drops it and counts it in `noise_stale`: it is a late duplicate of a handshake that has
   already finished.
4. On any other first message that passes the tag check, the responder draws a fresh ephemeral,
   computes `ee`, writes the second message with the key phase of §5.6, sends it to the source
   address of the first message, and keeps the result as its **pending** session. A pending
   session replaces any earlier pending one. It never replaces the current session.
5. The responder MUST NOT send a transport message on a pending session. It goes on using its
   current session, if it has one, until the pending one is confirmed.
6. The pending session is **confirmed** by the first transport message in its key phase that
   passes the tag check and the replay check (§5.5). The pending session then becomes current,
   and the old current one becomes previous.

**Roles do not swap.** A first message that reaches an initiator, or a second message that
reaches a responder, is dropped and counted in `noise_stale`. Two sides can never both initiate,
so the binding needs no tie-break.

**Peer address.** The initiator sends to its configured peer. The responder answers a first
message at that datagram's source address. It moves the address it sends its current session's
traffic to only on a transport message that passed the tag and replay checks. An unauthenticated
datagram never moves it, so a forged source address cannot redirect the session.

### 5.5 Transport messages: nonce and replay

**Normative.**

- **Nonce.** Each session keeps one 64-bit send counter per direction. It starts at 0 and goes up
  by one for each transport message sent, confirmations included. A nonce MUST NOT be used twice
  under one key. Concurrent senders on one link MUST draw nonces atomically.
- **Limits.** An initiator SHOULD start a rekey (§5.6) when its send nonce reaches 2^60. Neither
  side may send with a nonce at or above **2^62**: at that point the session is spent, the side
  stops sending on it, and the session is discarded as in §5.7. The bound sits far below Noise's
  reserved 2^64−1 and is never reached in practice.
- **Replay window.** Each session keeps the highest nonce it has accepted and a bitmap of the
  `W` nonces below it. `W` is a compile-time policy and MUST be at least 64. A receiver
  processes a transport message in this order:
  1. size and type (§5.3); a key phase with no session is counted in `noise_stale`;
  2. **replay pre-check:** a nonce at or below `highest − W`, or one already marked in the bitmap, is
     dropped and counted in `noise_replayed`, with no decryption;
  3. **tag check**, decrypting in place; a failure is dropped and counted in
     `noise_decrypt_failed`;
  4. **window update**, only now: a forged datagram can never move the window;
  5. the plaintext rule of §5.3; an empty plaintext stops here, and a frame is delivered.
- Messages may arrive out of order inside the window. Their order is not restored: a Tracer
  frame is self-contained, and ordering above the link is the protocol's own concern, as on the
  plain UDP link.

### 5.6 Rekey and the key phase

**Normative.**

- **A rekey is a new handshake.** The initiator starts one under the limits of §5.5, when the
  application's `now` passes `rekey_after` (§5.7), or whenever the application asks. Noise's
  in-place `Rekey()` (Noise §11.3) is not used: a new handshake gives the new session fresh
  ephemeral keys, and so forward secrecy against the old session's keys, at the cost of one
  round trip.
- **Only the initiator starts one.** A responder whose send nonce reaches the limit stops sending
  on that session, and waits for the initiator.
- **The responder picks the key phase.** If it has a current session, the new phase is that
  session's phase flipped. If it has none, the phase is 0. It sends the phase in the second
  message's encrypted payload, so the phase is authenticated. The initiator takes the phase from
  that payload.
- **Two sessions overlap, never three.** When a session becomes current, the one it replaces
  becomes **previous**. The previous session receives only and never sends, so frames that were in
  flight under the old keys still land. A responder discards its previous session when it creates a
  pending one, because the pending session takes the previous session's phase. An initiator
  discards its previous session when the new current one has the same phase (after the responder
  lost its state). These two rules keep a phase from ever naming two sessions, so a receiver never
  tries more than one key.
- **Keeping a previous session is optional.** A build MAY keep none, which is a compile-time
  policy that suits the narrowest targets. Messages in the old phase are then counted in
  `noise_stale` rather than delivered.

### 5.7 Time: the application's `now`

**Normative.** A Noise link MUST NOT read a clock or arm a timer. Every deadline is checked only
when the application passes `now`, a monotonic value in the application's own units, through the
link's poll seam. The link reports its next deadline, so the application knows when to call
again. The deadlines are kind-private config, in the same units as `now`:

| deadline | side | what happens when `now` passes it |
| ---- | ---- | ---- |
| retry | initiator | retransmit the first message (§5.4 step 2); the interval doubles on each retransmission, up to a kind-private cap |
| attempt | initiator | MAY end the attempt and start a new one with a fresh ephemeral key (§5.4 step 3) |
| pending expiry | responder | discard the pending session |
| `rekey_after` (age of the current session) | initiator | start a rekey (§5.6) |
| `reject_after` (age of the current session) | both | discard the session: the link goes down (§5.10) |
| previous expiry | both | discard the previous session |

A deadline set to 0 is never due. **Without `now` calls, nothing expires by time**, and each
operation resolves on its other events: a handshake completes when its second message arrives, a
pending session ends when another first message replaces it, and a session ends on a confirmed
rekey, on the nonce limit of §5.5, or on teardown. An application that never calls with `now` still
gets a working link. It gets no retransmission, so a lost handshake datagram waits for the next
demand.

### 5.8 Resources: the receiver pays, and nothing is allocated before the PSK is proven

**Normative.**

- **All session state is part of the link.** A Noise link holds a fixed number of slots: the current
  session, the previous session (optional, §5.6) and one handshake slot, which holds the
  initiator's attempt or the responder's pending session together with its stored second message.
  They are drawn from the **link's own source** when the link is built, and sized at compile time.
  A handshake fills a slot that already exists. Nothing on the receive path allocates.
- **Before the PSK is proven, a datagram costs the responder a bounded amount of CPU and no memory.** A first
  message is checked in the carrier's receive buffer: two HKDF calls, three SHA-256 updates and one
  16-byte tag check. Key generation and the Diffie-Hellman run only after the tag verifies. A first
  message that fails touches no state.
- **No library-internal buffers.** A transport message is decrypted **in place** in the receive
  segment the carrier drew from the link's source. Sealing a frame needs a destination: the link
  draws one segment of `frame + 25` bytes from its own transmit source, or seals in place when it
  owns the frame's bytes. When the source is exhausted, the frame is dropped and counted in
  `dropped_tx`: backpressure, never an out-of-memory failure.
- **The stored handshake messages** (the initiator's 49 bytes, the responder's 50) live in the handshake
  slot. They are part of the link's state, not a queue, and the binding queues no frame while a
  handshake runs: a send without a session fails (§5.1).
- **One peer per link.** This binding is for a point-to-point connection, which is what the
  `udp` kind is: one link, one peer, one session. A multi-peer listener would draw a session per
  peer from its own source **only after** the first message's tag check, and refuse newcomers when
  the source is exhausted rather than evict anyone. It is left to a later amendment (§15 Q3).

### 5.9 Failure: counted, never answered in clear

**Normative.** The binding has no error message, no reject, no cookie and no plaintext reply. The
only datagram a responder ever sends that is not a transport message is a second message, and it
sends one only to a first message whose PSK tag verified. Every failure is a silent drop on the
wire and one counter step:

| what arrived | counted in |
| ---- | ---- |
| reserved type; a handshake datagram of the wrong length; a transport datagram under 25 B or over `max_frame + 25`; a decrypted plaintext that is not exactly one frame | `malformed_rx` |
| a handshake message whose tag fails (wrong PSK, or tampered), or a second message with a bad key-phase byte | `noise_handshake_failed` |
| a transport message whose tag fails | `noise_decrypt_failed` |
| a transport message refused by the replay window | `noise_replayed` |
| a well-formed datagram for state this side does not hold: a phase with no session, a second message with no attempt in flight, a late first message, a message for the other role | `noise_stale` |

Because nothing is answered, an initiator whose PSK is wrong learns nothing from the responder. It
sees only a handshake that never completes. That is deliberate: a reply would tell a prober that
it reached a Noise responder and that its guess was wrong.

### 5.10 What the link reports

**Normative.** The connection vertex's value stays the RFC-0014 liveness byte. No new state is
added.

| role | state | on a Noise link it means |
| ---- | ---- | ---- |
| DIAL | `dormant` | no session, and no attempt in flight |
| DIAL | `dialing` | the first attempt is in flight, and there is no session yet |
| DIAL | `up` | a confirmed current session exists |
| DIAL | `reconnecting` | the link had a session, has lost it (rejected by age or by the nonce limit), and a new attempt is in flight |
| LISTEN | `listening` | the carrier socket is bound. As RFC-0014 says of every LISTEN link, this reports that the socket can be reached, not that a peer is attached |
| LISTEN | `bind-failed` | the carrier socket could not bind |

- A rekey that completes while the old session is still current does **not** pass through
  `dialing`. The link stays `up` across it.
- **Each new confirmed session is a session boundary.** It advances the connection vertex's
  generation exactly as a link going down with its tenancy kept does (RFC-0029 §8.2). A Noise link
  can therefore always report its session boundaries, and needs no per-boot epoch (RFC-0029 §8.2
  rule 4), even over UDP.
- **Subject.** The binding authenticates "a holder of this link's PSK" and nothing more. The frames
  a Noise link delivers carry the subject the application bound to that link through the subject
  seam (ADR-0018, ADR-0082). The binding grants no rights of its own. A frame is never delivered
  before the session exists, so no frame reaches the graph under an unauthenticated subject.

### 5.11 The `:stats.link.<child>` nouns

**Normative.** A Noise link adds these u64 nouns to its `:stats.link.<child>` block (reference/05
§The seam census record). Readers already MUST ignore unknown names, so the addition breaks no reader.

| noun | counts |
| ---- | ---- |
| `noise_handshakes` | sessions confirmed: on the initiator, second messages accepted; on the responder, pending sessions confirmed |
| `noise_handshake_failed` | handshake messages refused by the tag check or by the key-phase byte (§5.9) |
| `noise_retransmits` | first messages retransmitted (initiator), and stored second messages resent (responder) |
| `noise_decrypt_failed` | transport messages refused by the tag check |
| `noise_replayed` | transport messages refused by the replay window |
| `noise_stale` | well-formed datagrams for state this side does not hold (§5.9) |
| `noise_expired` | sessions discarded by `reject_after` or by the nonce limit |
| `noise_oversize` | frames refused at send for exceeding `max_frame` |

`dropped_rx`, `malformed_rx` and `dropped_tx` keep their meanings: receive-source exhaustion, a
peer's malformed datagram, and transmit-source exhaustion. A monitor reads an attack, a key mismatch or a
lossy path off the difference between two snapshots: a rising `noise_handshake_failed` with no
`noise_handshakes` is a wrong PSK or a prober, a rising `noise_replayed` is duplication or replay,
and a rising `noise_retransmits` is loss.

## 6. Security considerations

### 6.1 What the binding gives, and what it does not

**It gives** confidentiality and integrity of every frame on the hop, against an attacker who does
not hold the PSK; mutual authentication of "a holder of the PSK"; forward secrecy of each session
(§6.3); and replay refusal for transport messages.

**It does not give** identity. Anyone who holds the PSK can play either role toward anyone else
who holds it. If one PSK is installed on many boards, every one of them can impersonate every
other. The binding is meant for a recovery or provisioning step between two parties that share a
secret. A standing deployment moves to the identity patterns of #1993 (§7). The binding also does
not protect anything end to end (ADR-0045 Decision 5) and does not hide traffic sizes or timing.

### 6.2 The PSK is the whole secret

An attacker who records one handshake can test PSK guesses offline: each guess costs two HKDF
calls and one tag check against the first message. A PSK of 32 random bytes makes that hopeless.
A short or human-chosen secret does not. That is why §5.2 requires a deliberately slow
key-derivation function, applied by the application, for any PSK derived from a passphrase. The
library never derives one.

### 6.3 Forward secrecy and PSK compromise

The session keys depend on `DH(e_i, e_r)` and on the PSK. A PSK that leaks after a session has
ended does not decrypt that session, because its ephemeral keys are gone. A PSK that leaks while
an attacker is on the path lets the attacker run both handshakes and read everything after that
point. When a PSK is suspected leaked, the remedy is to replace it on both sides, and with
ephemeral keys that is enough for the future.

### 6.4 One PSK per link pair

Reusing one PSK across several link pairs lets a datagram recorded on one link be replayed to
another. On transport messages the replay window and the per-session keys defeat that. On a first
message they do not (§6.5). The prologue binds the binding version, not the link. One PSK per link
pair keeps a replayed first message to the link it came from.

### 6.5 First-message replay, and why no data rides in it

A first message is authenticated by the PSK but carries no freshness: there is no clock to put
a timestamp in it (WireGuard does exactly that). An attacker who recorded one can replay it at
any time. What that buys the attacker:

- the responder spends one key generation and one Diffie-Hellman, and sends one 50-byte second
  message, about the same size as the 49 bytes the attacker sent, so there is no amplification;
- the responder's **pending** session is replaced. A replay cannot touch the **current** session,
  because a pending session never replaces it until the initiator confirms it, and only the holder
  of the initiator's ephemeral key can do that;
- nothing is delivered. The attacker cannot produce the confirmation.

So a replay can delay a legitimate handshake that is racing it, which an attacker on the path can
do anyway by dropping datagrams. That is why the first message's payload MUST be empty (§5.2). In
`NNpsk0` that payload is encrypted but replayable and not forward secret, so a request in it (a
"0-RTT" recovery command) could be replayed and run again. Application data rides only in transport
messages, which a replay cannot reproduce.

### 6.6 Unauthenticated header bytes

The type byte and the nonce are not additional data. A changed nonce selects a different
ChaCha20 nonce, and a changed key phase selects a different key, so the tag fails in both cases.
A transport type changed to a handshake type, or the reverse, changes the length class and fails
§5.3. No header change can make a receiver accept a message it would otherwise refuse. The cost of
leaving them out of the associated data is nothing, and keeping the associated data empty keeps
the transport message exactly a Noise transport message.

### 6.7 Denial of service

- **Without the PSK:** two HKDF calls and one tag check per datagram, and no state. There is no
  amplification, because nothing is answered.
- **With a replayed first message:** one key generation and one Diffie-Hellman per datagram, and
  no allocation (§6.5).
- **Flooding transport messages:** one tag check each, and at most one key per message (§5.6). The
  replay pre-check refuses a known nonce without decrypting.
- No cookie exchange is needed (§13), because the expensive step already sits behind the PSK.

### 6.8 Implementation hazards

The primitives MUST be constant time where the host library offers it. Ephemeral keys MUST NOT be
reused across attempts: if both sides reused theirs, the session keys would repeat. A bad random
source breaks every guarantee here, and the application injects it, so the application owns it.
Key material in released slots SHOULD be wiped.

## 7. How KK and IK over the ADR-0045 identities follow

#1993 specifies the identity-bearing patterns over the node's ed25519 key, used in its X25519 form
(ADR-0086 Decision 8). This binding is the frame they attach to, and they reuse it unchanged
except for the handshake:

- **The same datagram framing.** Each pattern gets its own handshake type codes from the reserved
  range of §5.3. The transport types `0x04` and `0x05`, the nonce, the replay window, the key
  phase, the rekey rule, the `now` seam, the slots, the silent-failure rule and the counters are
  shared. So is a receiver's cheap first check, which is the length class.
- **The patterns.** `IK` (the initiator knows the responder's static key; ADR-0086), `XX` (neither
  knows the other's in advance: trust on first use; ADR-0086), and `KK` (both know each other's: a
  provisioned pair). The protocol names, prologues and payload layouts are #1993's.
- **The credential** rides in the handshake payload, where this binding carries only the key-phase
  byte. The key-phase byte keeps its place, as the payload's first byte.
- **The subject** comes from the static key through the verifier policy (ADR-0086 Decision 3)
  instead of the fixed per-link subject of §5.10.
- **The PSK property can be kept.** `KKpsk0` and `IKpsk2` add a PSK to an identity pattern. With
  `psk0` the cheap PSK check still comes before any Diffie-Hellman, so a site-wide PSK can gate the
  public-key work. Whether #1993 offers these modifiers is its own decision (§15 Q5).
- **Moving from NNpsk0 to an identity pattern** is a config change on both sides: a new pattern,
  the same carrier and the same connection vertex. Nothing on the graph side changes.

## 8. Cost: bytes, RAM and CPU across NARROW, MID and WIDE

Analytic, from the layouts. Nothing is measured yet (§12).

### 8.1 Bytes on the wire

25 bytes per frame: the type byte, the 8-byte nonce and the 16-byte tag.

| frame | plain UDP datagram | Noise datagram | overhead |
| ---- | ---- | ---- | ---- |
| 64 B | 64 B | 89 B | +39% |
| 256 B | 256 B | 281 B | +9.8% |
| 1 KiB | 1024 B | 1049 B | +2.4% |
| 4 KiB | 4096 B | 4121 B | +0.6% |
| 16 KiB | 16384 B | 16409 B | +0.15% (IP-fragmented on a 1500 B path, as the plain link is) |

Above 1 KiB the overhead is negligible. The cost that grows there is the IP fragmentation the
plain UDP link already pays (§5.3, path-MTU advice), and the binding neither adds to it nor
removes it. A handshake is 49 + 50 bytes, plus the 25-byte confirmation.

### 8.2 RAM per link

| slot | contents | about |
| ---- | ---- | ---- |
| session (current) | two 32-byte keys, send nonce, highest nonce, `W`-bit bitmap (`W` = 64), phase | 96 B |
| session (previous, optional) | the same | 96 B |
| handshake | initiator: ephemeral private key, `ck`, `h`, `k`, 49-byte message. Responder: a session, the 50-byte message, the initiator's ephemeral key | 180 B |
| **total** | | **about 370 B**, or about 280 B without a previous session |

These are fixed and drawn when the link is built. The receive buffer grows by 25 bytes over the
plain link's. A transmit segment of `frame + 25` bytes is drawn per send when the frame cannot be
sealed in place (§5.8).

### 8.3 CPU

- **Per frame:** one ChaCha20-Poly1305 seal or open over the frame. On a WIDE host that is far below
  the cost of the syscall. On a NARROW target without a ChaCha20 engine it is the dominant
  per-frame cost of the link, and grows linearly with the frame size, so above 1 KiB it should be
  measured before an implementation claims a figure.
- **Per handshake:** two X25519 scalar multiplications on each side (key generation and `ee`), plus
  a few HKDF calls. On a 160 MHz RISC-V MCU without an accelerator, a software X25519 costs
  milliseconds to tens of milliseconds (an estimate, to be measured in S3). It is paid once per session, not per frame.
- **Code size:** X25519, ChaCha20-Poly1305 and SHA-256, all of which an MCU SDK such as ESP-IDF's
  mbedTLS or PSA already ships. A build that does not select the binding links none of it.

### 8.4 Across the spectrum

- **NARROW:** no previous session, `W` = 64, `max_frame` within one carrier datagram: about 280 B per
  link and no allocation after build.
- **MID:** with a previous session, so frames in flight survive a rekey.
- **WIDE:** a larger `W` for high-rate links with deep reordering, chosen at compile time.

## 9. The constraints, checked

| constraint | how this RFC meets it |
| ---- | ---- |
| No timers or clock reads | Every deadline is checked against the application's `now` (§5.7). Without it, operations resolve on their other events. |
| No library-internal buffers | Decryption is in place. A sealed frame goes into a segment from the link's own source. The binding queues no frame during a handshake (§5.8). |
| Receiver pays | All session state is a fixed part of the link, from its own source. A peer without the PSK causes no allocation and no Diffie-Hellman (§5.8, §6.7). |
| Compile-time by default | The replay window `W`, keeping a previous session, and the crypto primitives are build-time choices. The PSK and the deadlines are per-link config. |
| Core stays crypto-free | The binding is a `security_noise` module over host primitives (ADR-0045, ADR-0086). The core sees an ordinary link. |
| Complexity by deletion | One framing, one rekey mechanism (a new handshake, not Noise `Rekey()` as well), no cookie path, no error path, and no fallback to plaintext. |
| Lookups population-independent | One peer per link, at most two sessions, and one key tried per message. |

## 10. Normative pages that change

### 10.1 A new binding page (normative annex)

A new reference page, `docs/reference/23-noise-link-binding.md`, carries §5 in full, together
with §6 marked informative and Appendix A as its worked example. `docs/spec/v1.md` §3 incorporates
it as an annex that applies **to a link that uses the binding**, and §1's sentence "a transport's
native framing below the TLV layer (each transport documents its own binding)" gains "except the
Noise link binding, which a link that uses Noise MUST follow (§3)". `docs/spec/index.md` §What is
normative restates the list. §15 Q1 asks whether to do this or to keep the RFC as the binding's
only text until #1993 extends it.

### 10.2 `docs/reference/05-protocol-tlvs.md` (normative annex)

§The seam census record, the `:stats.link.<child>` row: "…`dropped_rx`, `malformed_rx`,
`dropped_tx`, and, on a Noise link (RFC-0033 §5.11), `noise_handshakes`, `noise_handshake_failed`,
`noise_retransmits`, `noise_decrypt_failed`, `noise_replayed`, `noise_stale`, `noise_expired` and
`noise_oversize`."

In §Invalidation, the bullet on transports that cannot report a session boundary gains: "A Noise
link (RFC-0033) reports one at every confirmed session, so it carries no per-boot epoch."

### 10.3 Informative pages, records and the glossary

- **`docs/reference/10-module-catalog.md`:** `security_noise` moves from "future" to "specified
  (RFC-0033)", and its row says it wraps a datagram carrier.
- **`docs/reference/19-transports-are-vertices.md`:** one paragraph on a wrapping link. The
  connection vertex is the Noise link's, its value is the liveness of §5.10, and the PSK reference
  is kind-private config under the lean rule.
- **`CONTEXT.md`:** the "Noise link" term of §4, if a line is free.
- **#1649** (an ESP-IDF UDP link with Noise) builds its secure link against this binding, over the
  plain ESP-IDF `udp_link_t` it proposes.

### 10.4 Records this RFC contradicts, said explicitly

- **ADR-0086 Decision 8** says that a Noise link uses "handshake pattern **XX** or **IK**", and that
  "a Noise link that does not follow the binding is not a conforming Tracer link". This RFC adds
  `NNpsk0` as a sanctioned pattern of the same binding, for links with a PSK and no identity. At
  acceptance, ADR-0086 gains a dated amendment note: Decision 8's pattern list reads "XX or IK
  for links that carry identity (#1993), NNpsk0 for links that carry only a PSK (RFC-0033)". The
  rest of Decision 8 stands: a Noise link that follows neither is not conforming.
- **#1649's design comment** proposes a secure-link adapter **in core**, a **bounded session LRU**
  and a **stateless cookie retry**. This RFC places the adapter in the `security_noise` module,
  because the core stays crypto-free (ADR-0045 Considered options, ADR-0086 §Consequences). It
  refuses newcomers rather than evicting (§5.8), and it needs no cookie (§6.7). #1649's datagram
  mode (explicit counter, replay window, refuse a carrier too small for the overhead) is what §5.3 and
  §5.5 specify.
- **RFC-0014's LISTEN row** is kept as it is: a Noise responder reports `listening`, not `up`. §15 Q4
  asks whether that is wanted.

## 11. Breaking surface

None. The binding is optional and new. No frame, type code, field, conformance vector or
existing link changes. The new `:stats` nouns appear only on a Noise link, and readers already
ignore unknown names. A node that does not build the `security_noise` module links none of it.

## 12. Slices and conformance

### 12.1 Slices

| Slice | Contents | Gate |
| --- | --- | --- |
| S0 | This RFC, and its approval. | maintainer approval |
| S1 | The binding page and the annex edits of §10.1 and §10.2, in the form §15 Q1 rules. | docs build (`sphinx-build -n -W`) |
| S2 | The `security_noise` module on host: the Noise link over any datagram link, the `now` poll seam, the slots, the counters; the §12.3 tests over an in-memory datagram link and over UDP. The Appendix A vectors as a test. | full core-ci matrix; the vectors match byte for byte |
| S3 | ESP-IDF, through #1649: the same module over the ESP-IDF UDP link, with RAM per link and handshake time measured against §8. | ESP build and size gates; on-silicon handshake |

### 12.2 Conformance vectors

The conformance suite today round-trips frames, and a Noise transcript is not a frame. S2 adds a
**`noise/` category** whose cases are JSON transcripts: inputs (PSK, ephemeral private keys,
prologue, plaintexts) and expected outputs (each datagram's bytes, the handshake hash and the two
transport keys), as in Appendix A. A conforming Noise link reproduces every datagram byte for byte
from the inputs. The first case is Appendix A's. Two more follow: a key-phase-1 rekey, and a
second message whose key-phase byte is invalid, which the link must refuse.

### 12.3 Behaviour tests (at the wire, over an in-memory datagram link)

- The Appendix A transcript, end to end, between two Noise links.
- **Wrong PSK:** the responder counts `noise_handshake_failed`, sends nothing, keeps no state, and
  runs no X25519 (counted by the injected crypto seam). The initiator stays `dialing`.
- **Lost second message:** the initiator retransmits the identical first message on its `now`
  deadline, the responder resends the identical second message, and the handshake completes on
  one key pair.
- **No `now` calls:** with the first message lost, nothing is retransmitted, and the next demand
  completes the handshake.
- **Replayed first message** while a session is current: the current session keeps working, the
  pending one is never confirmed, and nothing is delivered.
- **Replayed and reordered transport messages:** every message inside the window is delivered once,
  and every duplicate or too-old one is counted in `noise_replayed`.
- **Tampering:** each flipped bit in the type, the nonce, the ciphertext or the tag is refused and
  counted, and none of them moves the replay window.
- **Rekey** under load: frames in flight under the old phase are delivered through the previous
  session, or counted in `noise_stale` on a build without one. The link stays `up`.
- **Oversize:** a frame of `max_frame + 1` bytes is refused at send and counted in `noise_oversize`.
  A datagram over `max_frame + 25` bytes is refused at receive and counted in `malformed_rx`.
- **Source exhaustion:** the transmit source is empty, so the frame is counted in `dropped_tx`
  and no allocation outside the source takes place.
- **No plaintext ever:** a send without a session fails as a link-down send, and the carrier
  carries no Tracer frame in clear.
- **Address:** a forged-source datagram that fails authentication never moves the responder's
  peer address, and an authenticated transport message from a new address does.
- **Generation:** each confirmed session advances the connection vertex's generation once.

## 13. Alternatives considered

- **Wait for #1993's IK and XX.** Rejected for this use case: they need static keys, and the
  recovery case has none yet (§2.1). This slice does not compete with them. It is the frame they
  reuse (§7).
- **`NNpsk2` or `NNpsk1`.** Rejected: the PSK would be checked only after the responder had
  generated a key and run a Diffie-Hellman, so anyone could make it do that work.
- **DTLS with a PSK cipher suite** (the catalog's `security_dtls` slot). Rejected for this slice:
  it is a larger state machine and a larger code footprint, it needs a cookie exchange of its own,
  and it does not compose with the raw ed25519 identities ADR-0045 chose for later.
- **Noise's stream framing** (a 2-byte length per message, implicit nonces). Rejected for a
  datagram carrier: one lost datagram desynchronises every implicit nonce after it.
- **A truncated nonce on the wire** (4 bytes, with the high bits implied). Rejected: it saves 4
  bytes per frame and adds a reconstruction rule with its own edge cases near the window. Revisit
  if a CAN FD or other small-MTU carrier needs the bytes.
- **WireGuard's receiver index** (4 bytes in every message). Not needed for one peer per link. The
  carrier's address and the key phase already pick the session. A multi-peer listener would bring
  it back (§15 Q3).
- **Noise's in-place `Rekey()`, in addition to a new handshake.** Rejected: two rekey mechanisms
  where one does, and the in-place one gives no new ephemeral keys.
- **A cookie or retry exchange against handshake floods.** Rejected: it protects a Diffie-Hellman
  that `psk0` already places behind the PSK.
- **Application data in the first message (0-RTT).** Rejected: it would be replayable (§6.5).
- **An error or reject message.** Rejected by ruling 7. It would also turn the responder into an
  oracle for probers.
- **Keying the prologue to the link.** Not adopted: binding the PSK to one link pair (§6.4) gets
  the same separation without needing both sides to agree on a link name.
- **A `noise_auth_failed` liveness state.** Rejected: the initiator cannot tell a wrong PSK from an
  absent peer, because nothing is answered, so the state would be a guess. Counters on the
  responder say it truthfully.

## 14. What would falsify this RFC

1. **An independent Noise implementation does not reproduce Appendix A.** The vectors were derived
   from the Noise text without an independent implementation to hand (Appendix A). A mismatch is
   a defect in this document, to be fixed before acceptance (§15 Q6).
2. **A carrier that cannot carry the binding without fragmenting.** That would be a deployment where
   `max_frame + 25` cannot fit the carrier's datagram and a frame cannot be made smaller. It would
   call for a fragmenting binding, which this one deliberately is not.
3. **A recovery flow that needs the responder to send first.** The responder cannot send until the
   initiator confirms (§5.4). If a real flow needs the board to speak first without the tool sending
   anything, the confirmation rule has to change.

## 15. Questions for the maintainer, each with a recommendation

1. **Where does the normative text live?** (a) A new reference page, incorporated by v1 §3 as an
   annex that applies to a link that uses the binding (§10.1). (b) This RFC alone, until #1993
   extends it into the one "Tracer over Noise" page. **Recommendation: (a), landed in S1.**
   ADR-0086 Decision 8 already rules that the binding is normative. An RFC is a change record, not
   the place a reader looks for the current rule. #1993 then adds its patterns to the same page.
2. **Amend ADR-0086 Decision 8's pattern list to admit `NNpsk0`?** **Recommendation: yes, at
   acceptance, with a dated amendment note** (§10.4): XX or IK for identity, NNpsk0 for PSK only,
   and one framing for all of them.
3. **One peer per link only, for now?** **Recommendation: yes.** The `udp` kind is point-to-point,
   and the recovery case is one tool and one board. A multi-peer Noise listener is a later
   amendment, with sessions drawn from the link's source after the PSK tag check, refusing newcomers
   rather than evicting, and probably as a bus session anchor (RFC-0031).
4. **Should a Noise responder report `up` while it holds a confirmed session?** RFC-0014 says that
   a LISTEN link reports `listening`: the socket can be reached, not that a peer is attached.
   **Recommendation: keep `listening`** and let the counters tell the story. A responder that
   needs to know when an authenticated peer arrives can watch `noise_handshakes`. A one-peer UDP
   listener could truthfully report `up`, but that would be the first LISTEN link to do so and
   would amend RFC-0014's LISTEN row for a single kind.
5. **Should #1993 offer `psk0` modifiers of its identity patterns** (`KKpsk0`, and an `IK` with a
   PSK)? **Recommendation: yes, as an option.** A site PSK in front of the identity handshake keeps
   the "no Diffie-Hellman before a secret is proven" property this binding has. It is #1993's
   decision. This RFC only keeps the framing ready for it (§7).
6. **The vectors' provenance.** No independent Noise implementation was available offline when
   this was drafted, so Appendix A was derived from the Noise text, over standard primitives (see
   Appendix A for how). **Recommendation: make "an independent implementation (for example the
   `snow` crate's stateless transport, or `noiseprotocol` for Python) reproduces Appendix A" a
   condition of acceptance,** or of S2 at the latest, and fix this document if it does not.

## 16. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the comment window is waived by default while
the project is solo-maintained. Sustained objections and their resolution are recorded in this
section as they arrive.

---

## Appendix A — Test vectors

### A.1 How they were produced

No independent Noise implementation was available offline when this RFC was drafted. The vectors
were therefore computed by a short Python script (A.4), written from the Noise text (revision 34:
§5 processing rules, §9 PSK handshakes, §11.4 out-of-order transport messages, §12 crypto
functions), over the `cryptography` package's X25519, ChaCha20-Poly1305 and HMAC-SHA-256.
The script checks itself in three ways:

- the initiator and the responder are separate state objects, and the script asserts that they reach
  the same handshake hash and the same transport keys, and that each opens the other's messages;
- every HKDF output is checked against an independent RFC 5869 HKDF (Noise's HKDF is RFC 5869
  with `salt = ck` and empty `info`);
- the initiator's ephemeral public key equals the public key that widely used TLS 1.3 walk-through
  examples publish for the private key `20 21 … 3f`, an X25519 result checked independently.

None of these checks would catch a misreading of the Noise token rules shared by both sides, which
is what §15 Q6 asks to close.

### A.2 Inputs

```
protocol name   Noise_NNpsk0_25519_ChaChaPoly_SHA256
prologue        547261636572206f766572204e6f697365207631      ("Tracer over Noise v1")
psk             000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
initiator e     202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f   (private)
responder e     404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f   (private)
frame i->r      0100040070696e67      VALUE "ping" (type 0x01, opt 0x00, len 4)
frame r->i      0100040070616e67      VALUE "pang"
key phase       0 (the responder had no current session)
```

### A.3 Outputs

Derived public values:

```
initiator e.pub 358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd166254
responder e.pub 79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a
DH(e_i, e_r)    04c304fb1ca83cee75e206344231f33797e07d9929db670994b7c6fbeb1dc255
```

The symmetric state after each step (`ck`, `h`, `k`; `k` is the cipher key the step leaves):

```
InitializeSymmetric
  ck f0b890826fb8e6584af6948d227169c381c9ed6a4c1c90df4b9ff454fa947f7b
  h  f0b890826fb8e6584af6948d227169c381c9ed6a4c1c90df4b9ff454fa947f7b
  k  (empty)
MixHash(prologue)
  ck f0b890826fb8e6584af6948d227169c381c9ed6a4c1c90df4b9ff454fa947f7b
  h  7e5554a8f4a6abb452fc4d771075d6beac9b01cb254da598ca336c0c59f6aa33
-> psk: MixKeyAndHash(psk)
  ck 9b4711ec9740e2ed66c6b7dd318498a099fb3af117ed91c3afe7be34409d7e10
  h  f17ba3006857f586a12770439732f0743e0a6828120276209ecc13c03a0f63a5
  k  a70be24398b39bb16173ac9e9915f5e476934de93c561f8706ecfb65d1d70192
-> e: MixHash(e.pub), MixKey(e.pub)
  ck 4bbe7fe7eab33e763d98f1fe9317d8897a7a9a04a88c24044826dbc2c6dc328d
  h  db820b8ecaf5fb8251d2c02ade5a068902ced283abd5a09a5a3b8fc81a4aca84
  k  9dd2bab4f776313d2482a17e73fdb37c763a50befe22035eed9d712ba42064f7
-> payload: EncryptAndHash(empty)
  h  cb5d6f217b02910d189b0e534b13f41ff53c2fd89d7fda34c3858f34fdc183e4
<- e: MixHash(e.pub), MixKey(e.pub)
  ck b807d66b184d23acaf8f449ba584cfdd795dc30e9397383d32de3a5cace355f0
  h  494f0fbe2754a925c1f1da14b099fabaf7ec171be95459a62ff9af86fc521ed1
  k  8451c478b7eb0f6e2665d44af680e48923b413c2acc8f6568b8d5028c4ce897e
<- ee: MixKey(DH(e, re))
  ck 6b8a5bc652b3f9778cf22aa99918204288e05858977cd52ca474b347ac1e27c1
  k  fb638e73cc98b3390ce667b57f857412953104f938ffa43e949945f0dbcc6adc
<- payload: EncryptAndHash(00)
  h  8903e859524104142f5c0a2ec5a1105598b0c06a9618c5019c1ba0b264ca2362
```

The datagrams, type byte first:

```
msg1 (49 B)  01
             358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd166254
             1b5fa40e22b306f2c72d3a5a3a8e6fb5
msg2 (50 B)  02
             79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a
             96 4cbf62aaf3483220cd624c22f37e1c55
```

(In `msg2`, `96` is the encrypted key-phase byte and the last 16 bytes are its tag.)

After `Split()`:

```
handshake hash  8903e859524104142f5c0a2ec5a1105598b0c06a9618c5019c1ba0b264ca2362
k_i2r           810cde8f99b1a2a5a863e41b93fd1c11b29ab8fee3f3edcb662e163b245164ca
k_r2i           f8ca9cf382ec15ac3609fa976595baafec96af6729b05edab8e41839ed09f6cd
```

Transport messages (type `04` = key phase 0, then the nonce, the ciphertext and the tag):

```
confirmation, initiator -> responder, n = 0, empty plaintext (25 B)
  04 0000000000000000 b215c81b607068db3638292cc0825965

frame "ping", initiator -> responder, n = 1 (33 B)
  04 0100000000000000 843c7ef02f4b3184 8add30c2b08aa1eef0985b2201f08621

frame "pang", responder -> initiator, n = 0 (33 B)
  04 0000000000000000 8402810c9207079d 65de2bc6ba0de7827759a468234362ac
```

### A.4 The generator

Run with Python 3 and the `cryptography` package. It prints A.3 and fails if the two sides
disagree.

```python
import hashlib, hmac, struct
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

NAME, PROLOGUE = b"Noise_NNpsk0_25519_ChaChaPoly_SHA256", b"Tracer over Noise v1"
PSK, E_I, E_R = bytes(range(0, 32)), bytes(range(32, 64)), bytes(range(64, 96))
PING, PANG = bytes.fromhex("0100040070696e67"), bytes.fromhex("0100040070616e67")

def sha(*p): return hashlib.sha256(b"".join(p)).digest()
def nonce(n): return b"\0" * 4 + struct.pack("<Q", n)
def pub(k): return X25519PrivateKey.from_private_bytes(k).public_key().public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw)
def dh(k, p): return X25519PrivateKey.from_private_bytes(k).exchange(X25519PublicKey.from_public_bytes(p))

def hkdf(ck, ikm, n):
    tk, out, prev = hmac.new(ck, ikm, hashlib.sha256).digest(), [], b""
    for i in range(1, n + 1):
        prev = hmac.new(tk, prev + bytes([i]), hashlib.sha256).digest()
        out.append(prev)
    assert b"".join(out) == HKDF(hashes.SHA256(), 32 * n, salt=ck, info=b"").derive(ikm)
    return out

class Sym:
    def __init__(s):
        s.h = NAME.ljust(32, b"\0") if len(NAME) <= 32 else sha(NAME)
        s.ck, s.k, s.n = s.h, None, 0
        s.mix_hash(PROLOGUE)
    def mix_hash(s, d): s.h = sha(s.h, d)
    def mix_key(s, ikm): s.ck, s.k = hkdf(s.ck, ikm, 2); s.n = 0
    def mix_key_and_hash(s, ikm):
        s.ck, th, s.k = hkdf(s.ck, ikm, 3); s.mix_hash(th); s.n = 0
    def e(s, p): s.mix_hash(p); s.mix_key(p)            # psk handshake: e also calls MixKey
    def enc(s, pt):
        ct = ChaCha20Poly1305(s.k).encrypt(nonce(s.n), pt, s.h); s.n += 1; s.mix_hash(ct); return ct
    def dec(s, ct):
        pt = ChaCha20Poly1305(s.k).decrypt(nonce(s.n), ct, s.h); s.n += 1; s.mix_hash(ct); return pt

def seal(k, n, pt): return b"\x04" + struct.pack("<Q", n) + ChaCha20Poly1305(k).encrypt(nonce(n), pt, b"")
def unseal(k, d):
    n = struct.unpack("<Q", d[1:9])[0]; return n, ChaCha20Poly1305(k).decrypt(nonce(n), d[9:], b"")

i, r = Sym(), Sym()
i.mix_key_and_hash(PSK); i.e(pub(E_I)); m1 = b"\x01" + pub(E_I) + i.enc(b"")
r.mix_key_and_hash(PSK); r.e(m1[1:33]); assert r.dec(m1[33:]) == b""
r.e(pub(E_R)); r.mix_key(dh(E_R, m1[1:33])); m2 = b"\x02" + pub(E_R) + r.enc(b"\x00")
i.e(m2[1:33]); i.mix_key(dh(E_I, m2[1:33])); assert i.dec(m2[33:]) == b"\x00"
(k1, k2), (r1, r2) = hkdf(i.ck, b"", 2), hkdf(r.ck, b"", 2)
assert (k1, k2, i.h) == (r1, r2, r.h)
c, f, g = seal(k1, 0, b""), seal(k1, 1, PING), seal(k2, 0, PANG)
assert unseal(r1, c) == (0, b"") and unseal(r1, f) == (1, PING) and unseal(r2, g) == (0, PANG)
for name, v in (("msg1", m1), ("msg2", m2), ("hash", i.h), ("k_i2r", k1), ("k_r2i", k2),
                ("confirm", c), ("ping", f), ("pang", g)):
    print(name, v.hex())
```
