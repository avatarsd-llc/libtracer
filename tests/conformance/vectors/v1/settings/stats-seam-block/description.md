# settings/stats-seam-block

[RFC-0010](../../../../../../docs/spec/rfcs/0010-owner-app-fields-and-schema.md) §Amendment 1
(2026-08-22, [#1503](https://github.com/avatarsd-llc/libtracer/issues/1503) step 5) — the
answer to `read <any-vertex>:stats.graph.delivery` on a fresh node: **one seam, one block,
one TLV**.

```
SETTINGS (PL=1) {
  NAME "no_target"          VALUE u64 = 0
  NAME "denied"             VALUE u64 = 0
  NAME "out_of_memory"      VALUE u64 = 0
  NAME "fan_out_truncated"  VALUE u64 = 0
}
```

```
0B406D00020009006E6F5F746172676574010008000000000000000000
0200060064656E696564010008000000000000000000
02000D006F75745F6F665F6D656D6F7279010008000000000000000000
0200110066616E5F6F75745F7472756E6361746564010008000000000000000000
```

`4 (header) + 109 (payload) = 113 bytes`; a trailer rides per the serving link's egress
policy, as for any reply.

**What the vector is for.** The whole counter block is sampled in **one call** and served as
**one TLV**. That is the only shape under which `core/STYLE.md` §Introspection's
snapshot-coherence clause can hold — six separate per-counter fields could never be mutually
coherent, which is why individual-counter fields are a named door and not this surface.
Members are `NAME`/`VALUE` pairs in the declaration order of the underlying POD, each a
**fixed-width u64 little-endian**; a reader MUST ignore unknown `NAME`s and MUST NOT depend
on the member count (the seam vocabulary is `core/STYLE.md` §Introspection's, and a seam may
grow a noun). A **mem-class** seam (`:stats.mem.control`, `:stats.mem.ring`) carries the
five-noun block — `capacity` / `in_use` / `peak` / `refused` / `largest_refused` — in the
same shape.

**The other three answers are byte-identical to vectors already held**, and are deliberately
not duplicated here — duplicating them would claim the census carries per-surface detail it
does not:

| the read | the answer | held as |
| --- | --- | --- |
| a recognised seam, caller the ACL admits | these bytes | this vector |
| a recognised seam, caller the ACL **denies** | `ERROR{tr::access::denied}` (`0x0050`) | [`acl/denied-caller-undeclared-app-field`](../../acl/denied-caller-undeclared-app-field/description.md) |
| an **unrecognised** `:stats` spelling (bare `:stats`, `:stats.mem`, `:stats.mem.nope`, any `[N]`), **any** caller | `ERROR{tr::schema::not_found}` (`0x0031`) | [`settings/removed-knob`](../../settings/removed-knob/description.md) |
| an **AWAIT** carrying a `:stats` selector | `ERROR{tr::schema::not_found}` (`0x0031`) | [`settings/removed-knob`](../../settings/removed-knob/description.md) |

The second and third rows together are the amendment's disclosure rule: **NAME validity
resolves above the READ gate** (so an unrecognised spelling is caller-independent — no
spelling has two answers split by who asked, the RFC-0010 erratum's protocol-owned row),
while the **VALUE resolves below it**. That inverts `:identity`, which is pre-auth by design:
a node's memory census is not first-contact material, so a denied caller is refused and
thereby learns the seam exists — intended, and no more than the published `settings` /
`children` namespaces already disclose. The fourth row needs no new rule at all: every
field-tailed AWAIT has answered `SCHEMA_NOT_FOUND` unconditionally since
[#585](https://github.com/avatarsd-llc/libtracer/issues/585), and counters do not bump
`write_seq_`, so nothing could ever fire such a wait.

**Behavioural binding** (see [`../../../HARNESS.md`](../../../HARNESS.md) § *What a vector
gates*): `core/tests/stats_field_test.cpp` pins these bytes byte-exact, the node-scoped
invariant (unrelated vertices, and a vertex created afterwards, return byte-identical
blocks), the gate inversion with `:identity` as its control, every unrecognised spelling at
**both** an admitted and a denied caller, and the read-only half (a write answers
`SCHEMA_NOT_FOUND`, whoever asks). The AWAIT row is bound by
`core/tests/op_resolve_test.cpp` — `test_await_field_selector_is_enotty`, which drives the
`:stats.mem.control` selector through the wire resolver beside its field-less control.
