# settings/stats-seam-net-router

[RFC-0010](../../../../../../docs/spec/rfcs/0010-owner-app-fields-and-schema.md) §Amendment 2
(2026-08-23, the [#1503](https://github.com/avatarsd-llc/libtracer/issues/1503) residual) —
the answer to `read <any-vertex>:stats.router.drops` on a node whose router has forwarded
nothing: **one seam, one block, one TLV**, in exactly the Amendment 1 §D.3 shape.

```
SETTINGS (PL=1) {
  NAME "flatten_dropped"       VALUE u64 = 0
  NAME "forward_iov_dropped"   VALUE u64 = 0
  NAME "arena_dropped"         VALUE u64 = 0
  NAME "assemble_dropped"      VALUE u64 = 0
  NAME "reply_iov_dropped"     VALUE u64 = 0
  NAME "delivery_iov_dropped"  VALUE u64 = 0
  NAME "malformed_rx"          VALUE u64 = 0
}
```

```
0B40E000
02000F00666C617474656E5F64726F70706564010008000000000000000000
02001300666F72776172645F696F765F64726F70706564010008000000000000000000
02000D006172656E615F64726F70706564010008000000000000000000
02001000617373656D626C655F64726F70706564010008000000000000000000
020011007265706C795F696F765F64726F70706564010008000000000000000000
0200140064656C69766572795F696F765F64726F70706564010008000000000000000000
02000C006D616C666F726D65645F7278010008000000000000000000
```

`4 (header) + 224 (payload) = 228 bytes`; a trailer rides per the serving link's egress
policy, as for any reply.

**What the vector is for.** The BYTES are Amendment 1's shape and nothing about them is new
— that is the point. What Amendment 2 changes is **where the block comes from**. The
router's counters are per-`fwd_router_t`, not per-graph, so L4 cannot reach them and
Amendment 1 stopped the census at the graph; Amendment 2 inverts the DIRECTION instead of
the dependency, and the router registers a sampler UP into the graph
(`graph_t::configure_stats_sampler`) beside the five `{fn, ctx}` seams its constructor
already installs. The wire shape therefore stays one encoder and one block, and L4 still
names nothing below it.

**The other answers this surface gives** are byte-identical to vectors already held and are
deliberately not duplicated:

| the read | the answer | held as |
| --- | --- | --- |
| a served net seam, caller the ACL admits | these bytes | this vector |
| a served net seam, caller the ACL **denies** | `ERROR{tr::access::denied}` (`0x0050`) | [`acl/denied-caller-undeclared-app-field`](../../acl/denied-caller-undeclared-app-field/description.md) |
| any net-plane spelling on a node with **no router constructed** | `ERROR{tr::schema::not_found}` (`0x0031`) | [`settings/removed-knob`](../../settings/removed-knob/description.md) |
| `:stats.link.<name>` naming **no registered child**, or a **removed** one | `ERROR{tr::schema::not_found}` (`0x0031`) | [`settings/removed-knob`](../../settings/removed-knob/description.md) |
| an unserved NAME in a net class (`:stats.router.nope`), **any** caller | `ERROR{tr::schema::not_found}` (`0x0031`) | [`settings/removed-knob`](../../settings/removed-knob/description.md) |

The last three rows are the amendment's normative sentence: **a seam whose sampler is
unregistered, and a de-registered link's sub-key, answer `SCHEMA_NOT_FOUND`** — which
Amendment 1 §Compatibility already spells as "this node does not publish that seam", never
as an error. The third-from-last row is also §D.2 unchanged across the new classes: because
the sampler decides validity, that decision is taken **above** the READ gate through a
recognition probe that samples nothing, so no net-plane spelling has two answers split by
who asked.

**Behavioural binding** (see [`../../../HARNESS.md`](../../../HARNESS.md) § *What a vector
gates*): `core/tests/stats_net_seam_test.cpp` pins these bytes byte-exact, the LIVE sampling
(a provoked `malformed_rx` appears in the block a later read returns), the per-link identity
of `:stats.link.<child>` across two links with different counters, the unregistered and
removed link sub-keys, the no-router positive control, node scoping, the caller-independence
of an unserved net NAME with a served one as its control, and the read-only half.
