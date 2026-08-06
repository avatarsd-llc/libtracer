# Ad-hoc range-slice addressing (`/path[a..b]`)

libtracer does not add ad-hoc range-slice addressing — e.g. `/hw/ws2812[0..29]` as a
SUBSCRIBER target addressing elements 0..29 of one array-valued vertex.

## Why this is out of scope

The proposal ([#336](https://github.com/avatarsd-llc/libtracer/issues/336)) surfaced during
the 2026-07-08 architecture grilling, when the adopter side (strawberry-fw device-graph
migration) *appeared* to need range-slice binding targets. The need dissolved on inspection:

1. **Named groups are expressible today via field promotion** (CONTEXT.md *Field promotion*).
   A named zone is a *promoted child vertex* (`/hw/ws2812/zone_a`) whose slice spec lives in
   its device-owned settings; delivered writes are applied at the offset by the device's own
   logic. This is legal under load-bearing claim 5 (the graph imposes no shape on user data)
   and costs **one vertex per named group** — not one per element, and no addressing-grammar
   change.

2. **Field-change notification** — the other gap from the same session — is likewise covered
   by promotion: a promoted child is an ordinary vertex, so it subscribes and notifies like
   any other, with no new wire surface.

Adding `[a..b]` to the addressing grammar would introduce a second way to express what
promotion already expresses, buying no new capability while enlarging the normative path
grammar every implementer must parse — against the project ordering (minimalism above
unification).

## The adjacent legitimate need, and where it is served

An application that genuinely wants *ad-hoc, caller-chosen* sub-ranges (not a fixed set of
named zones) can still express them **app-side**: the slice bounds travel in the payload the
device applies at its offset, exactly as strawberry-fw's own hardware logic already does.
The graph stays shape-agnostic; the app owns the slice semantics.

Reconsider only if a concrete need appears that promotion provably cannot express — an
addressing case where the set of ranges is unbounded and caller-chosen *and* must be visible
to the graph's routing/ACL layer (not just the device). No such case has been shown.
