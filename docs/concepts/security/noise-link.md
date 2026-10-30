# Noise links

## In one paragraph

A Noise link carries Tracer frames inside a Noise session over a datagram carrier, UDP on a
host. Both ends share a 32-byte pre-shared key and nothing else: no certificate, no static key,
no pinning. The pattern is `Noise_NNpsk0_25519_ChaChaPoly_SHA256`. The first handshake message
proves the key before the responder does any public-key work, and the session is forward secret,
so a key that leaks later does not open a recorded session. Each frame then travels as one
encrypted transport message, 25 bytes larger than the frame, and the frames the router sees are
the same bytes it would see on a plain link. The link never sends a frame in clear, never answers
a failure, and reads no clock: every deadline runs on the `now` the application passes it.

## The picture

```{mermaid}
:caption: "Legend: vermillion = peer, blue = frame."
flowchart LR
    accTitle: A Noise session from first message to the first reply
    accDescr: The initiator, the DIAL side, sends a 58-byte first message that carries the PSK proof. The responder checks its tag before any Diffie-Hellman and answers with a 50-byte second message. The initiator's first transport message is its request, which confirms the session, and the responder's reply acknowledges it; only then does the initiator report up. Each later frame is one transport message, 25 bytes larger than the frame.
    I[[initiator, DIAL]]:::peer ==>|"UDP: first message, 58 B"| R[[responder, LISTEN]]:::peer
    R ==>|"second message, 50 B"| I
    I ==>|"request: the confirmation"| T[/"frame + 25 B"/]:::frame
    T ==> R
    R ==>|"reply: the acknowledgement, then up"| I
```

## When you need it

When frames cross a network you do not trust and the two ends can be given the same secret: two
boards on a shared Wi-Fi, a gateway and its devices, a host and a tool. Use it where TLS would
need certificates the devices cannot manage.

## How it works

**The key comes from the application.** A `SPEC` that creates a Noise link names the PSK by
reference; the application's key store resolves it. The key never appears in a frame, a
`SETTINGS` record or a `:` field, and no read returns it. The ephemeral keys come from a random
source the application injects, fresh for every attempt and every answer.

**The handshake.** The DIAL side starts an attempt when the link is demanded and has no session:
a send fails at once, counted in `dropped_tx`, and starts one. The first message carries a
counter that must rise across attempts, and the responder keeps the highest it has accepted, its
mark, so a recorded first message is refused once a newer counter has been accepted. A request
and its reply take two round trips, and the link reports `up` only after the reply.

**Transport messages.** Each carries its 64-bit nonce in clear and is opened in place in the
receive buffer. A 64-message replay window refuses a duplicate. A frame is never split and never
packed with another; one larger than `max_frame` is refused at send, counted in
`noise_oversize`.

**Silence on failure.** A wrong key, a tampered message, a replay or a stale message is dropped
and counted, and nothing is sent back, so a prober learns nothing. The ten `noise_*` counters
are readable as the link's `:stats.link.<child>` record.

**Time.** `poll(now)` runs the retry, attempt and pending deadlines on the application's clock
and returns the next one. A deadline of 0 is never due. Without `poll`, nothing expires and
nothing is retransmitted; a lost datagram waits for the next demand.

**The subject.** The link proves only that the far end holds this link's key. Its frames carry
the subject the application binds to the link through the [subject lookup](identity.md).

## Cost

### Wire bytes

25 bytes per frame: a type byte, the 8-byte nonce and a 16-byte tag. The handshake is one
58-byte and one 50-byte datagram, then the confirmation and acknowledgement. A 1 KiB frame
becomes a 1,049-byte datagram; the largest frame over IPv4 UDP is 65,482 bytes.

### Allocations

All session state is one block from the link's `memory.state`, drawn when the link is built,
so a first message under a wrong key costs one hash, one key derivation, one tag check and no
memory. Each send draws one `frame + 25` block from `memory.io`; when that is exhausted the
frame is dropped and counted.

### Latency

`bench_noise_link` measures request and reply, streaming and setup against the plain UDP link at
64 B to 64 KiB, and `bench_noise_crypto` the cost of each crypto backend; `bench/run_noise.sh`
runs both. This page quotes no figure.

### Flash and RAM per target

| Target | Shipped | Note |
| --- | --- | --- |
| NARROW | no | |
| MID | not yet on ESP-IDF | `bench/esp_noise_crypto` measures the mbedTLS backend's flash |
| WIDE | host, over UDP | backend chosen at build time: OpenSSL, libsodium or mbedTLS |

## What the library does not do

- It does not rekey a session or expire one by age yet, and keeps no previous session.
- It does not persist the initiator's counter or the responder's mark yet.
- It does not serve more than one peer on a link.
- It does not protect a frame past the hop that decrypted it.
- It does not hold the key, the entropy or the clock; the application supplies each.

## Pitfalls

- **Symptom:** the DIAL side stays `dialing` and `noise_handshake_failed` rises on the responder.
  **Cause:** the two ends resolve different keys. **Fix:** provision the same 32-byte key.
- **Symptom:** after the initiator reboots, its handshakes are refused and
  `noise_handshake_replayed` rises. **Cause:** its counter restarted below the responder's mark.
  **Fix:** derive the counter from a persisted boot count, or clear the mark.
- **Symptom:** a lost handshake datagram is never resent. **Cause:** the application does not
  call `poll`. **Fix:** call it with `now` by the deadline it returns.

## Where the rules live

**Normative source:** [reference 23](../../reference/23-noise-link-binding.md#1-what-the-binding-is-and-where-it-sits),
the annex [v1 §3](../../spec/v1.md#3-wire-format) incorporates for a link that uses the binding.

**Exercised by:** `nnpsk0-appendix-a`, `noise_link_test`, `security_noise_test`.

## History

:::{dropdown} How the Noise link got here
- [RFC-0033](../../spec/rfcs/0033-noise-nnpsk0-datagram-link-binding.md) (accepted 2026-10-10)
  defined the binding; its Amendment 1 made the implementation hazards normative.
- Version 0.19.1 shipped the crypto backends, and 0.20.0 the host link.
:::

Next concept: [Concurrency](../concurrency.md). Go deeper:
[reference 23 §8, resources](../../reference/23-noise-link-binding.md#8-resources-the-receiver-pays-and-nothing-is-allocated-before-the-psk-is-proven),
then [§9, failure](../../reference/23-noise-link-binding.md#9-failure-counted-never-answered-in-clear),
then [datagram links](../transports/datagram.md).
