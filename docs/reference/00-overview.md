# Reference 00 — Protocol Overview

> **Status**: stub. Canonical content lives in [../plans/00-vision-and-reality-check.md](../plans/00-vision-and-reality-check.md) until promoted.

## What goes here when filled

- One-page definition of what libtracer **is** as a wire+graph protocol, independent of any implementation.
- Conformance levels (minimal node, full node with discovery, bridge node, recorder node).
- Versioning rule and the wire-format-version bit.
- Glossary of terms used throughout the reference (cross-link to [../plans/99-glossary.md](../plans/99-glossary.md)).
- The five load-bearing claims that any conforming implementation must honor:
  1. Same TLV substrate from memory through wire through graph.
  2. Read/write only API surface; subscribe via field-write.
  3. No wire-level fragmentation; address-shift slicing only.
  4. Bridges are first-class; every host is a router.
  5. Type code allocation respects the core/user split (`0x00–0x7F` reserved, `0x80–0xFF` user).
