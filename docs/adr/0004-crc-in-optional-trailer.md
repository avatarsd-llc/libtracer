# CRC lives in the optional append-only trailer (opt.CR), CRC-32C default

Frame integrity is a **trailer** field, present iff `opt.CR=1`, **appended at egress and stripped at ingress**, computed over `payload + trailer_ts` bytes (the header is excluded). Width is selected by `opt.CW`: **CRC-32C** (Castagnoli, default) or CRC-16-CCITT; both are mandatory for conforming receivers, and a mismatch returns `ERROR=CRC_FAIL`. CRC is a bit-flip detector, **not** adversarial integrity.

## Considered options

- **CRC in the header** (the obvious design; the pre-spec C++ does this). Rejected: a header-resident CRC changes on every bridge re-emit and would break the payload-bytes-invariant guarantee the same-substrate model rests on. A trailer that is append-only at egress / strip-only at ingress keeps payload bytes byte-identical at rest, in transit, and on re-emit.
- **The pre-spec XOR-16 fold.** Rejected: not a real CRC (the legacy glossary already calls it "broken").

## Consequences

- Payload bytes are invariant across rest / transit / re-emit — the property bridging and recording depend on.
- The `core/` code's in-header `uint16_t` XOR-16 is non-conformant on algorithm, width, **and** placement.
