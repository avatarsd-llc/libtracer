# Retire type code 0x05 (LIST); nesting is opt.PL=1 on a purpose-specific type byte

There is **no generic structured-container type**. Type code `0x05` (formerly `LIST`) is permanently retired and never reused; senders MUST NOT emit it, and receivers treat it as reserved-but-unassigned. Any TLV with `opt.PL=1` is a structured container of concatenated child TLVs, and its **type byte declares what the children mean** (PATH, SUBSCRIBER, POINT, ROUTER, SETTINGS, …). An array-whole read (e.g. `read('/x:subscribers[]')`) returns a `PL=1` reply whose children are the element TLVs; an **atomic multi-field write uses SETTINGS (`0x0B`)**.

## Considered options

- **Keep a generic `LIST` container** (earlier drafts; the legacy glossary still calls it "the graph-node-as-TLV mechanism"). Rejected: a container with no semantic of its own forced the type byte to be meaningless at L3, and every real use already has a specific purpose.

## Consequences

- The type byte becomes a proper L3 concern; structured-ness is signalled solely by `opt.PL`.
- The retirement sweep is incomplete and must finish: remove surviving "LIST" references in `core/`, the legacy glossary, and the **reference itself** (`03-addressing.md`, `06-user-data-packing.md`, and the dead `01:273` cross-ref).
