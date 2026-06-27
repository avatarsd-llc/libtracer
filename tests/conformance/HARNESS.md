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

The **C++ reference is golden**: when the wire changes, it blesses new/updated
vectors and every other core must match them.

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
- **TypeScript / Rust**: pending — implement this contract when the native core lands.

## Differential fuzzing (cross-core drift, beyond the curated vectors)

The curated vectors pin a dozen hand-picked wire shapes. To catch drift those
points *miss*, [`diff_fuzz.py`](diff_fuzz.py) generates random **valid** frames
from an explicit seed and round-trips each through **both** cores, asserting
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
