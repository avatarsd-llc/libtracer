# Reference 06 — User Data Packing into the Graph

> **Status**: stub. Canonical content lives in [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) until promoted.

## What goes here when filled

- Rules for packing user payloads as TLVs:
  - Single scalar → user-range (`0x80–0xFF`) TLV with raw bytes.
  - Structured record → `LIST` of named-children (each child is `NAME` + value-TLV pair).
  - Variable-length collection → `LIST` of homogeneous TLVs.
  - Large payload → address-shift across `ep[0..N]` with shared `TIME`.
- The same-substrate insight, made operational:
  - In-memory representation is a `buffer_chain<view>` tree where each TLV is a view (pointer + length) into a parent buffer.
  - Mix / split / concat at the graph level rearrange views — no byte movement.
  - Serialization = walking the view tree; the walk produces the same bytes regardless of how the tree was assembled.
  - **Proof obligation**: any sequence of mix/split/concat operations on a view tree must yield byte-identical output if serialized.
- Ownership transfer at delivery:
  - The receiving endpoint takes ownership of the top-level buffer (refcount transfer; no memcpy).
  - Sender's reference is consumed; subsequent fanout to additional subscribers shares the same buffer via refcount inc.
- Worked examples:
  - 5-byte RC control packet → single `VALUE` TLV.
  - 1 KB sensor record with 4 named fields → `LIST` of 4 children.
  - 64 KB camera frame → 16× 4 KB slices on `ep[0..15]` with shared `TIME` TLV; ownership transfer per slice.
- Iterative parser pattern (TLV_MAX_DEPTH=32) — safe on 4 KB MCU stacks; recursion forbidden.
