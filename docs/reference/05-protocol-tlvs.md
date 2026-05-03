# Reference 05 — Protocol-Defined TLVs

> **Status**: stub. Canonical content lives in [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) until promoted.

## What goes here when filled

For **each** TLV in the reserved range `0x00–0x7F`, byte-precise spec:

- **`VALUE` (0x00)** — opaque payload, no protocol semantics.
- **`NAME` (0x01)** — UTF-8 name fragment.
- **`DESCRIPTION` (0x02)** — UTF-8 free-form description, optional in any vertex.
- **`SUBSCRIBER` (0x03)** — subscriber slot record: peer id, QoS profile, liveness state, filter (if any).
- **`LIST` (0x04)** — ordered nested-TLV container; the graph node primitive.
- **`PATH` (0x05)** — UTF-8 path string.
- **`POINT` (0x06)** — coordinate or scalar point (numeric).
- **`ERROR` (0x07)** — error report (code + UTF-8 message); used in STATUS replies.
- **`STATUS` (0x08)** — vertex status snapshot (subscriber count, last-write-ts, liveness, error tail).
- **`ACL` (0x09)** — access-control list entry.
- **`SETTINGS` (0x0A)** — QoS / configuration block (RELIABILITY, DURABILITY, HISTORY, DEADLINE, LIVELINESS).
- **`TIME` (0x0B)** — 64-bit ns-since-Unix-epoch timestamp; either embedded via `opt.TS` or as a child of a LIST.
- **`ROUTER` (0x0C)** — bridge / router metadata (peer id, transport identifier, hop count, route cost).

Format per entry:

1. Byte table (offset / size / field / encoding).
2. Hex example with annotations.
3. Where it appears (which flows in [04-communication-flows.md](04-communication-flows.md) emit / consume it).
4. Required vs optional sub-fields, defaults, validation rules.
5. Future-extension reservation note (where new fields can be added without breaking parsers).
