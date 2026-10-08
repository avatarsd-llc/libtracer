# subscriber/no-target

[RFC-0021](../../../../../../docs/spec/rfcs/0021-wire-subscriber-target-frame-of-reference.md)
§4.D and §G.4, as restated by its erratum of 2026-10-08: **on a routed `:subscribers[]`
append, `target_path` is optional; the return route is the target.**

```
SUBSCRIBER (PL=1) { }                  ; no children: no target_path
```

```
04400000
```

**Behavioural expectation.** Carried as the payload of a routed subscribe, a
`FWD{ op=WRITE, dst=<producer>, FIELD :subscribers[], src=<return route>, SUBSCRIBER{} }`
arriving over a link:

- the append is admitted and answered `REPLY{kind=RESULT}`;
- a later write at the producer is delivered as `FWD{WRITE}` over the arrival link, with
  `dst` = the accumulated `src` of the subscribe (the return route).

The same bytes written through the local `:subscribers[]` or `:subscribers[N]` field door are
refused `ERROR{tr::schema::type_mismatch}` (`0x0030`): that door delivers to a local target,
and the record names none. The routed append needs no target because its delivery already
rides the return route; a `PATH` child there is read only when it routes through a mount
(RFC-0021 §4.B.1).

## Why this is an erratum, not a change

Reference/05 §`0x04` labelled `target_path` "required" in the record layout. RFC-0021 §4.D
already said a `SUBSCRIBER` with no `PATH` child binds to the arrival session and delivers
along the accumulated `src`, and `graph_t::subscribe_wire` (`core/src/graph.cpp`) has always
admitted one. The label now says where the target is required. No wire byte moves.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md): these 4 bytes are a well-formed TLV, so
every core round-trips them. The admission is bound in `core/tests/op_resolve_test.cpp`,
`test_routed_append_needs_no_target`: the routed append answers `RESULT`, the producer's next
write reaches the remote sink on the arrival link with the subscribe's `src` as its return
route, and the ablation writes the same bytes through the local field door and gets
`TYPE_MISMATCH`. The bindings check that their SUBSCRIBER target reader finds no target in
these bytes.
