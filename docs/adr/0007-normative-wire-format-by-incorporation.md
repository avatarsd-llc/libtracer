# Normative wire format lives in reference/01 + 05, incorporated by reference from the spec

Status: accepted

Rather than duplicate the byte-precise wire format into the normative spec, `docs/spec/v1.md` §3 **incorporates `docs/reference/01-data-format.md` and `05-protocol-tlvs.md` by reference**, and those two reference sections are relabelled **normative**. The reference suite remains the single source of the wire-format bytes; the spec is a thin normative shell that points at it and adds only the conformance language (§3.1 path handles, §4 vectors, §5 versioning).

## Considered options

- **Promote/move the byte-precise content up into a self-contained spec.** Rejected for now: maintaining the bytes in two places risks exactly the spec-vs-reference drift this consolidation is fixing. (Revisit at the freeze gate if a self-contained spec is wanted for second implementers.)
- **Leave the spec stubbed.** Rejected: conformance (§4) would have no normative anchor at all.

## Consequences

- `docs/reference/01` and `05` win over `03`/`06` wherever they clash on wire format, and their "Status: descriptive" line changes to normative.
- The reference-internal cracks (LIST survivors, `ERROR 0x08` packing, `VERSION_MISMATCH/opt.VR`, `io_dir_t`) **must be fixed before or as** those sections become normative — a contradiction in a descriptive doc is a nuisance, but in a normative one it is a conformance bug.
