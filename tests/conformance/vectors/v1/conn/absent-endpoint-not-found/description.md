# conn/absent-endpoint-not-found

RFC-0014 §6's **creatability probe**, on the wire: a remote `read /net/can/conn:schema`.
Against a node that declares no `can` module the reply carries
`ERROR{tr::path::not_found}` (`0x0020`) — which an orchestrator reads as *"no creator
endpoint here, this transport is not creatable on this node."*

```
FWD{ op=READ,
     dst=/net/can/conn,
     FIELD{ NAME "schema" },
     src=/reply-ep }
```

```
0f403100010001000006000d00036e65740363616e04636f6e6e10400a000200
0600736368656d6106000900087265706c792d6570
```
(one byte string, wrapped here for width; the canonical bytes are `input.bin`.)

| Bytes | Meaning |
| --- | --- |
| `0f 40 31 00` | FWD, `opt.PL=1`, body length 49 |
| `01 00 01 00 00` | VALUE u8 `0x00` — `op = READ` |
| `06 00 0d 00` | PATH, `opt = 0`, packed body length 13 |
| `03 6e6574` `03 63616e` `04 636f6e6e` | `dst` = `net` / `can` / `conn` (RFC-0018 packed `[u8 len][bytes]` segments) |
| `10 40 0a 00` `02 00 06 00 736368656d61` | FIELD, `opt.PL=1`, `NAME "schema"` — the `:schema` selector |
| `06 00 09 00` `08 7265706c792d6570` | `src` = `reply-ep` |

## Why the probe exists at all

RFC-0014 §3 **hides** `conn` from `/net/<module>:children[]` — that listing answers the
module's member *connections*, and the control that creates them is not one of them. So
enumeration can never tell a peer that a creator endpoint is there. §6 closes that with the
probe: read the endpoint's `:schema` and read the answer as the capability.

The two halves are inseparable, and both live in this one frame's `dst`: `conn` must be
**unlisted** and must stay **addressable**. Hiding it in a way that also made it unresolvable
would break the discovery contract that exists *because* it is hidden.

## Why `tr::path::not_found` and not `tr::schema::not_found`

The distinction is RFC-0014 §Compatibility's, and it is not cosmetic:

- **`tr::path::not_found` (`0x0020`)** — the endpoint `/` vertex is **absent**. There is no
  door. This is the missing-`/`-vertex answer, matching RFC-0009 §A.1.1 (*"`/net/unexport` is
  `PATH_NOT_FOUND` like any other unbuilt path"*), and it is the one this vector pins.
- **`tr::schema::not_found` (`0x0031`)** — the endpoint is **present** and refused a config
  type it does not know (RFC-0014 §2). That is the ADR-0021 ENOTTY pattern for an unsupported
  `:field`, not for a missing vertex.

An implementation that collapsed them would tell an orchestrator "this board cannot do CAN at
all" when the truth was "that config key is unknown" — or the reverse, sending it into a retry
loop against a door that does not exist.

## The bytes are not inherently an error

Nothing in this frame says "failure". Sent to a node that **does** declare a `can` module, the
identical bytes answer a `RESULT` carrying the module's `:schema`. The error is entirely a
property of the *receiving node's* declarations, which is exactly the fact the probe reports —
and it is why the binding test drives these bytes at both kinds of node.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md) — the harness drives no topology and
resolves nothing, so it can only prove the frame round-trips. The behaviour is bound in
`core/tests/transport_vertex_test.cpp`, `test_conformance_vectors`: these exact bytes are
arena-decoded and resolved through the production `tr::graph::op_resolver_t`, the reply's
`STATUS{ ERROR{ VALUE u16 } }` is read out and asserted to be `0x0020`, and the **ablation**
— the same node with `register_module("can", …)` called — answers `RESULT` over the same
bytes.
