# Conformance harness contract

Every libtracer core (C++, TypeScript, Rust, …) is kept from drifting by running the
**same** vectors under `tests/conformance/vectors/v1/` and proving byte-identical
behavior. The mechanism and rationale are in
[ADR-0028](../../docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md).
This file is the contract a core implements to join that gate.

## The vectors

Each case is a directory under `vectors/v1/<category>/<case>/` containing:

| File | Meaning |
| --- | --- |
| `input.bin` | The canonical wire bytes for the case. |
| `expected.json` | Human-readable / cross-language decoded form (the spec of what `input.bin` *means*). |
| `description.md` | Prose describing the case. |

A **negative case** carries `reject.bin` **instead of** `input.bin` (exactly one of
the two, never both): bytes the codec MUST refuse to decode. Its `expected.json`
has a top-level `"reject": "<ERROR_NAME>"` naming the required decode error using
the same stable names as the `--roundtrip` `ERR:<name>` strings (e.g.
`FRAME_INVALID`). The distinct filename keeps every `input.bin` consumer (benches,
arena tests, the coverage audit) on valid frames only.

The **C++ reference is golden**: when the wire changes, it blesses new/updated
vectors and every other core must match them.

## What a vector gates — and what it does NOT

A vector gates the **codec, and only the codec**. The harness contract below is
`encode(decode(input.bin)) == input.bin`, which is satisfied by *any* well-formed TLV —
including one whose bytes no implementation ever produces or acts on. Passing a vector
therefore proves that a core can carry those bytes across its decoder and encoder without
losing a bit. It proves **nothing** about what the core does when it receives them.

This is not a gap to be closed by making the vectors cleverer; it is a boundary. Two
consequences follow, and both are load-bearing:

1. **A vector whose `description.md` states a behaviour is making a claim the harness
   cannot check.** Reverting the feature the behaviour names leaves every harness at a
   full score. So the claim must additionally be made where it *can* be false — a host
   test in the implementation that owns the behaviour, which fails when the behaviour is
   removed. Every behavioural vector below names that test.
2. **A core may score `ok` on a vector whose behaviour it does not implement at all.** A
   pure-codec binding round-trips a `SUBSCRIBER` carrying a `delivery_policy` word without
   ever interpreting a bit of it. That is a correct `ok` — the codec really is
   conformant — and it is why the score alone must never be read as "this core implements
   the feature". `run-all.py` reports codec agreement; the table below is where the
   behavioural gate lives.

Behavioural conformance is gated by **each implementation's own host suite**, not here.

### The execution model has ONE forwarder — a named limitation

The harness drives no topology at all: it decodes and re-encodes `input.bin`. There is therefore
no way to express a *routed* vector here, and the shortfall is not merely that a vector cannot
route — it is that until #419 **every FWD test in the C++ tree also had exactly one forwarder**.
That is how the multi-hop `dst` form could disagree three ways (reference docs, vectors,
implementation) for two weeks without a single red light: no artifact in the repo ever consumed
one address at two nodes.

`fwd/fwd-routed-two-mount` is delivered as a **codec-tier** vector for that reason — its bytes are
the origin frame of a real two-mount route, and the route itself is proven where it can fail, in
`core/tests/fwd_two_mount_test.cpp`. Read the vector's `ok` as "the codec carries these bytes",
never as "this core routes them".

### Behavioural vectors and their binding tests

| Vector | Behaviour claimed | Bound by |
| --- | --- | --- |
| `subscriber/policy-absent` | A `SUBSCRIBER` naming no `delivery_policy` behaves exactly as a pre-RFC-0022 one: no latch on join, later writes still delivered. | C++ `core/tests/qos_policy_test.cpp` — `test_policy_absent_is_todays_behaviour`, `test_conformance_vectors`, `test_replace_door_latches`. TS `bindings/typescript/packages/client/test/vectors.test.mjs`. |
| `subscriber/policy-durability` | Bit 5 set ⇒ the producer's latched value is delivered once on join, for **this** subscription and not its siblings — through the append door and the RFC-0009 §D.1 replace door alike. | C++ `qos_policy_test.cpp` — `test_policy_durability_is_per_subscriber`, `test_conformance_vectors`, `test_replace_door_latches`. Rust `bindings/rust/src/structured.rs` and TS `vectors.test.mjs` gate only the packed-word codec — neither binding has a graph or latch, so the delivery behaviour is bound by the C++ suite alone. |
| `subscriber/policy-reserved-bits` | Bits 6–15 are **ignored, never rejected**; they do not leak into `reliability`/`priority`; and the record round-trips through `:subscribers[N]` verbatim. | C++ `qos_policy_test.cpp` — `test_policy_reserved_bits_are_ignored`, `test_conformance_vectors`. Rust `structured.rs` tests. TS `vectors.test.mjs`. |
| `settings/removed-knob` | A write of any withdrawn `:settings.<knob>` answers `ERROR{tr::schema::not_found}`, caller-independently, and these are the bytes the resolver builds. | C++ `qos_policy_test.cpp` — `test_removed_knobs_are_schema_not_found`, `test_removed_knob_reply_bytes`; `core/tests/acl_test.cpp` — `test_flat_knob_surface_is_withdrawn` (read **and** write, three callers). |
| `settings/read-container-shape` | A bare `:settings` read serves `SETTINGS{ NAME "app", SETTINGS{…} }` — the container survives, the knobs do not. | C++ `qos_policy_test.cpp` — `test_settings_container_keeps_its_shape`, `test_conformance_vectors` (byte-exact against `read_settings`). |
| `settings/schema-enumerates-nothing` | `:schema`'s synthesized protocol part enumerates zero knobs, and the owner part still follows it. | C++ `qos_policy_test.cpp` — `test_schema_enumerates_nothing`, `test_conformance_vectors` (byte-exact against `read_schema`). |
| `stream/history-depth-host-only` | The STREAM ring depth is owner-side (`set_history_depth` trims the ring) and has **no** wire surface: read and write both answer these bytes. | C++ `qos_policy_test.cpp` — `test_history_depth_is_host_only`, `test_removed_knob_reply_bytes`. |
| `field/field-nested` | `:settings.app` is the two-level field that survives §3.B; a second level that is not `app` resolves to nothing. | C++ `qos_policy_test.cpp` — `test_conformance_vectors` (the vector's own bytes fed to `op_resolver_t` as the selector, with the sibling spelling as the ablation). |
| `tlv-types/point-schema-app` | `read_schema` emits `POINT{ NAME, SETTINGS, NAME "app", SETTINGS }` for a vertex with a declared app table. | C++ `qos_policy_test.cpp` — `test_conformance_vectors` (byte-exact). |
| `fwd/fwd-routed-two-mount` | A `dst` crossing TWO `net/<module>/<name>` mounts is consumed by two different routers — `strip_k = 3` at each — and only the residual `/sensor/temp` resolves at the third node; `src` grows by each forwarder's FULL mount run. | C++ `core/tests/fwd_two_mount_test.cpp` — three in-process `fwd_router_t` nodes chained by loopback link pairs, wired through the production `transport_vertex_t` `:children[]` SPEC door. No binding gates it: neither the Rust nor the TypeScript core has a router, so both correctly score `ok` on the codec alone. |
| `fwd/fwd-src-accumulated` | The reverse half of the same invariant, seen mid-route: after two hops `src` has grown by **two full** `net/<module>/<name>` mount runs (six segments, most recent first), never by two bare NAMEs, while `dst` still carries the next mount plus its residual. | C++ `core/tests/fwd_two_mount_test.cpp` — the hop-2 probe asserts `src == /net/downlink/a/net/downlink/cli/reply-ep` byte-exactly, the identical two-run shape this vector carries. Same binding caveat as the row above: no Rust/TS router. |

`tlv-types/settings-reliability` is **not** in this table on purpose: since RFC-0022 it pins
a `SETTINGS` record *shape* and no longer names any protocol knob, so it is a pure codec
vector with no behaviour to bind.

When a core does not implement a vector's behaviour, the honest state is *not* a silent
`ok` on the codec score plus an empty row here — it is either a binding test in that core's
suite, or an explicit note in [`harnesses.json`](harnesses.json) that the surface is absent.

## What a harness must do

A harness is a single command:

```
<harness> <vectors-dir>
```

For **every** case directory under `<vectors-dir>` that contains an `input.bin`, it
MUST check, at minimum, the **round-trip**:

> `encode(decode(input.bin)) == input.bin`   (byte-for-byte)

A harness SHOULD additionally check the **semantic** form (`decode(input.bin)`
matches `expected.json`) where the language has a JSON parser to hand.

A decode that fails, or a round-trip that differs by one byte, is a `not ok`.

For every case directory that contains a `reject.bin` (negative case), it MUST
check instead that

> `decode(reject.bin)` **fails**, with the error named by `expected.json`'s
> `"reject"` field

A decode that *succeeds*, or that fails with a different error, is a `not ok`.
Both kinds of case are folded into the same sorted TAP list, keyed by directory.

## Output: TAP version 13

The harness writes [TAP](https://testanything.org/) to **stdout**, one line per
vector, keyed by the vector's path **relative to `<vectors-dir>`** (so keys match
across cores):

```
TAP version 13
1..3
ok 1 - crc/value-crc32c
ok 2 - framing/empty-status-ok
not ok 3 - path/path-sensor-temp
```

- The `1..N` plan line states how many vectors were run.
- The description after `-` is the vector's relative path (no leading `./`), forward
  slashes on every platform.
- Exit code: `0` if every vector is `ok`, non-zero otherwise.
- Diagnostics may be written to **stderr** (ignored by the driver) or as TAP
  `# comment` lines.

## Registering the harness

Add an entry to [`harnesses.json`](harnesses.json). The driver
[`run-all.py`](run-all.py) appends `<vectors-dir>` to your `cmd`, runs it, parses the
TAP, and folds it into the cross-core matrix. Set `"enabled": false` while a core is
still a stub — it then shows as *pending* and does not gate.

## Reference harnesses

- **C++** (golden): `core/tests/conformance_runner --tap <vectors-dir>`.
- **TypeScript** (pure-TS core): `node bindings/typescript/packages/core/conformance/harness.mjs <vectors-dir>`.
- **Rust** (native `no_std` + alloc core): `cargo run --manifest-path bindings/rust/Cargo.toml --example conformance -- <vectors-dir>`.

All three are registered and `enabled` in [`harnesses.json`](harnesses.json), so the driver gates on their cross-core agreement.

## Differential fuzzing (cross-core drift, beyond the curated vectors)

The curated vectors pin a set of hand-picked wire shapes. To catch drift those
points *miss*, [`diff_fuzz.py`](diff_fuzz.py) generates random **valid** frames
from an explicit seed and round-trips each through **all three** cores, asserting
byte-for-byte agreement (ADR-0028 / ADR-0032 extended from curated points to a
fuzzed corpus). A mismatch is a genuine cross-core bug; the failing seed + bytes
are printed so the frame can be promoted to a curated vector.

```
# build the C++ core first (or set $LIBTRACER_CXX_HARNESS), then:
python3 tests/conformance/diff_fuzz.py            # 1000 seeds (default)
python3 tests/conformance/diff_fuzz.py --seeds 5000
python3 tests/conformance/diff_fuzz.py --start 4242 --seeds 1   # reproduce one seed
```

It drives a small **`--roundtrip`** batch hook each harness implements alongside
`--tap`: read one hex frame per stdin line, print the `decode`→`encode`
re-encoding as hex (or `ERR:<reason>` on a decode failure), one line per input.
A new core joins the differential gate by implementing that hook.
