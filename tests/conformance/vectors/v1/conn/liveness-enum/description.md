# conn/liveness-enum

RFC-0014 §3/§4: a connection vertex's **value is its link-liveness state**, and the state is a
**one-byte `VALUE`**. These are the bytes a freshly created engine-managed `DIAL` connection
holds, and the bytes `read /net/<module>/<name>` serves and a `subscribe` on it propagates.

```
VALUE{ u8 0x00 }   ; link_state_t::DORMANT
```

```
0100010000
```

| Bytes | Meaning |
| --- | --- |
| `01 00 01 00` | VALUE, `opt = 0`, payload length 1 |
| `00` | `DORMANT` — the vertex exists, refcount is 0, there is no socket |

Five bytes. There is no envelope, no `SETTINGS` wrapper and no dedicated liveness TLV type: the
liveness state **is** the vertex value, which is what makes it readable, `await`-able and
subscribable through the operations that already exist (RFC-0014 §3, last bullet) rather than
through a new verb.

## The encoding this vector blesses

RFC-0014 §4's enum table, **in table order**, starting at zero. The RFC deferred the byte
values under the clause-kind rule — `transport_vertex.hpp`'s `link_state_t` says so in as many
words ("the byte encoding becomes normative on the S7 conformance-vector merge") — so this
vector plus the shipped enum are what make them normative.

| Byte | `link_state_t` | Role | Meaning |
| --- | --- | --- | --- |
| `0x00` | `DORMANT` | DIAL | vertex exists; no socket (refcount 0) |
| `0x01` | `DIALING` | DIAL | a connect attempt is in flight (first-ever or resumed) |
| `0x02` | `RECONNECTING` | DIAL | retrying toward `UP` between backoff waits |
| `0x03` | `UP` | DIAL | socket connected, bidirectional |
| `0x04` | `LISTENING` | LISTEN | listen socket bound and accepting |
| `0x05` | `BIND_FAILED` | LISTEN | the listen socket could not bind |

Three properties of that table are load-bearing, and each is the reason a byte could not have
been assigned differently:

- **`DORMANT` is `0x00`, and that is a migration constraint, not a coincidence.** The enum
  supersedes the binary `set_link_state(name, bool)`, whose "down" was the falsy default. A
  resting link therefore keeps reading `0` for anything that only ever asked "is it up?", and
  the one value in the table that a peer sees most often costs nothing to encode.
- **`UP` is `0x03`, not `0x01`.** The order is the RFC's table order (the DIAL lifecycle, then
  the LISTEN pair), not a frequency or a truthiness ranking. A core that assigned the enum by
  "up = 1" would round-trip this vector unchanged and still be wrong on the wire, which is why
  the bound test below enumerates all six against the live door rather than checking this file.
- **Six values, and no seventh.** In particular there is **no `healing` state**. RFC-0014 §4's
  prose twice describes "the transient states `dialing`/`healing`", which names no enumerator —
  the retry state is `RECONNECTING` (`0x02`), and §4's own table has always said so. That prose
  is corrected by the RFC-0014 errata batch
  ([#1475](https://github.com/avatarsd-llc/libtracer/pull/1475)); this vector pins the six that
  exist. A core reading the phantom as a seventh state would have to place it somewhere in this
  table, and every placement moves at least one already-shipped byte.

`BIND_FAILED` (`0x05`) is assigned by no code path in the reference implementation today — an
eagerly constructed `LISTEN` socket that fails to bind fails the create instead — but it is a
value a peer must be able to read, so it is pinned here and driven through the public
`set_link_state` door by the bound test rather than left to the first implementation that needs
it to choose.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md), and here the boundary is unusually sharp:
a five-byte `VALUE` carrying one byte round-trips in **any** conforming core regardless of what
that byte means to it, so this file on its own cannot tell `DORMANT = 0x00` from any other
assignment. The mapping is bound in `core/tests/link_liveness_test.cpp`,
`test_conformance_vectors`, which drives a live connection vertex through **every one of the six
states** via the production doors and reads the published byte back off `graph_t::read`.

The transitions themselves — `dormant→dialing→up`, `up→reconnecting→up`, `listen→listening` —
are bound by that same suite's engine tests; the `refcount-0→dormant` clauses are a separate
HARNESS.md row with **no vector directory**, because RFC-0014 §4.1 makes *when* an op-woken
socket closes a MAY, and only the three MUSTs are conformance surface.
