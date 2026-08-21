# conn/remove-reserved-rejected

RFC-0014 §2/§3's `remove-reserved-rejected`: a `NAME` write to `/net/<module>/conn` naming the
reserved endpoint name itself. **The endpoint cannot self-destruct** — the write is refused and
never reaches `retire()`.

```
NAME{ "conn" }
```

```
02000400636f6e6e
```

| Bytes | Meaning |
| --- | --- |
| `02 00 04 00` | NAME, `opt = 0`, payload length 4 |
| `636f6e6e` | `"conn"` — the reserved, protocol-owned name |

## Reserved on both sides of the control

`conn` is reserved per module, and RFC-0014 §3 refuses it twice with two different identities,
which is why this vector and `conn/spec-name-in-use` are separate cases:

- **Create-side.** A `SPEC` naming `conn` collides with the endpoint vertex already occupying
  that key, so it fails `tr::path::in_use` (`0x0022`) at `register_vertex_key` — the same
  answer any occupied name takes.
- **Remove-side (these bytes).** The refusal is *not* about occupancy: the name resolves
  perfectly well. It is a refusal to route a protocol-owned vertex to `retire()` at all, so it
  is an access decision and takes `tr::access::denied` (`0x0050`).

Getting this wrong is not a cosmetic error: an endpoint that retired itself would remove the
only door through which a peer can create connections under that module, and RFC-0014 §6 makes
that endpoint's `:schema` the sanctioned creatability probe — so the module would report
"not creatable" forever after, with no wire path back.

## A note on the identity's spelling

RFC-0014 §2 writes this refusal as `ERROR{tr::acl::permission_denied}`. That spelling is **not
in RFC-0002 §D's registered table**; the registered identity is `tr::access::denied`, code
`0x0050`, which is what `status_t::PERMISSION_DENIED` maps to in `core/src/fwd_reply.cpp` and
therefore what a peer actually reads. Under RFC-0014 §Discussion's clause-kind rule an error
identity is normative once pinned by code **plus** a conformance vector, so this vector pins
`0x0050`; the RFC's prose spelling is an erratum, not a second code. Raised on
[#492](https://github.com/avatarsd-llc/libtracer/issues/492).

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md). Bound in
`core/tests/transport_vertex_test.cpp`, `test_conformance_vectors`: the write is refused with
`PERMISSION_DENIED`, the registered wire code for that status is asserted to be `0x0050`, the
endpoint still resolves, and — the non-vacuous half — a `SPEC` written to it afterwards still
creates, so the refusal left a working door rather than a wrecked one.
