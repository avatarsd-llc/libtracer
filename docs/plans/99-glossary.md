# 99 — Glossary (SUPERSEDED)

> **Status**: superseded, 2026-06-24. This glossary predated the [docs/reference/](../reference/) suite and taught the pre-spec (v0.0) wire model — LEB128 length, finite-pool mode, `VR`/`FP` `opt` bits, an in-header CRC / XOR-16, a generic `LIST` (`0x05`), and a privileged "Core". All of that is wrong under protocol v1.

The canonical vocabulary now lives in **[/CONTEXT.md](../../CONTEXT.md)** (the root context glossary), which tracks the reference suite and the normative spec. Read it first.

- For wire-format terms (`opt` byte, TLV header, length, trailer, CRC, structured TLV): [/CONTEXT.md](../../CONTEXT.md) and [docs/reference/01-data-format.md](../reference/01-data-format.md) + [05-protocol-tlvs.md](../reference/05-protocol-tlvs.md).
- For the design rationale these plans capture: the other `docs/plans/*` documents remain valid as *history* (why the protocol looks the way it does), but are not the byte-level reference.

The pre-spec content of this file is preserved in git history (before 2026-06-24) for anyone tracing the v0.0 → protocol-v1 migration.
