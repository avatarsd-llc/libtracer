# Transport framing modes: full-TLV, header-elided (transport-native addressing), and advertise+id-match — chosen by the adapter, uniform to the bridge

"First-ready-to-strawberry" requires a **zero-overhead swap over the existing CAN and WebSocket buses**: drop libtracer in as the io_layer without regressing latency or bandwidth. The flagship — **100 ksps over CAN** — makes the constraint sharp: a 9-byte sample cannot afford a 4-byte TLV header (+44% on a bandwidth-bound bus). Yet load-bearing claim 4 says **bridges are core and transport choice is invisible** — the router must stay uniform. This ADR reconciles the two by making *framing* a transport-adapter concern with three coexisting modes.

## Decision

A **transport adapter** chooses its on-wire framing mode; the **bridge/router is uniform across all of them** (it only ever sees full TLVs) and never performs an identity↔path lookup. Three modes:

1. **Full-TLV ("full caps").** Self-describing frames carry the full PATH + control surface. Enables discovery, dynamic paths, in-band creation/ACL — the whole feature set. Used on capable transports (IP/WS) where a header is negligible, and for occasional control frames everywhere.

2. **Header-elided ("non-interactive bindings" / transport-native addressing).** The transport keys on its **native frame identity** (a CAN ID, a WebSocket channel). A **static `identity↔path` map held by the transport adapter** (transport config, *not* bridge state) lets the adapter **synthesize the PATH header on ingress** and **elide it on egress** — so the TLV header **never hits the constrained bus** and the existing CAN/WS frames are byte-unchanged (literally zero added overhead). The bridge above it is stateless: it receives a full, synthesized TLV indistinguishable from one off IP.

3. **Advertise + id-match.** A full-TLV **advertise** frame *establishes* a binding at runtime: `id ↔ path` for a single value, or `group-id ↔ (path, slice structure)` for a payload split across frames. Subsequent **lean, id-matched** frames carry the values/slices. The group form is **[ADR-0011](0011-address-shift-totality-opt-in.md) address-shift slicing made dynamic** — the advertise frame is the manifest the ADR otherwise carries as a static `expected_count`/`:manifest`. So one mechanism spans a 9-byte elided sample → a GB advertised rope group.

The modes **coexist**: **per-deployment** (an elided CAN leaf bridged to a full-TLV IP backbone — the **bridge is the stateless translation point**) and **per-transport** (occasional full-TLV control/advertise frames establish the elided data bindings — exactly the `discovery_static` vs `discovery_mdns` split). "Full caps" *sets up* "non-interactive bindings."

**Zero-copy boundary.** Eliding/assembling is zero-overhead *on the wire*, but **zero-copy of large elided or rope payloads requires the transport to hand each frame up as an owning/borrowable `view_t` — the M6 view-delivering seam.** The current flat-span `tr::Transport` receiver forces a per-frame synthesis copy; for small samples (9 bytes at 100 ksps ≈ 1.3 MB/s memcpy) it is negligible, for GB rope groups it is not. So **advertise+id-match (a graph-level protocol) composes with M6 (a transport-level capability)**; neither obviates the other.

## Considered options

- **Full-TLV on every transport.** Rejected: the +4-byte header makes 100 ksps-over-CAN infeasible and is not a *zero-overhead swap* of strawberry's existing frames — it re-frames them.
- **Header-elided on every transport.** Rejected: loses self-description — no discovery, no dynamic paths, no in-band control surface. "Full caps" needs full-TLV somewhere.
- **Put the identity↔path map in the bridge.** Rejected: makes the bridge stateful and transport-aware, directly violating claim 4. The map belongs in the adapter, which uniforms addressing *before* the bridge sees a frame.
- **A separate, bespoke "compact" wire format.** Rejected: advertise+id-match reuses the existing PATH/manifest machinery and unifies single-value and rope cases; a second format would fork the protocol.

## Consequences

- Strawberry's existing CAN/WS frames are swapped **without re-framing or added overhead** (header-elided); capable links keep **full caps** (full-TLV); both interoperate through the stateless bridge.
- The router/bridge stays **stateless and transport-agnostic** (claim 4 preserved); framing-mode + the `identity↔path` map are **transport-adapter** state, established statically (`discovery_static`) or dynamically (an advertise frame).
- **One mechanism (advertise+id-match)** scales from a 9-byte elided value to a dynamic GB rope group; it **composes with the M6 view-delivering seam** for end-to-end zero-copy.
- New per-transport responsibilities: a framing-mode selection and an `identity↔path` map; reference 10 (transports) and the deployment-profile doc record which transports default to which mode.
- This is a **transport-adapter / framing** decision; it adds no core wire-format type beyond reusing PATH and the (reserved) `MANIFEST` advertise frame, so no RFC against the immutable spec is required.
