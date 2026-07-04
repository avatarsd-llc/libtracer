# Whole-stack architecture review and deepening plan — 2026-07-04

> **Status**: session record (descriptive). The *decisions* live in [ADR-0047](../adr/0047-build-time-closed-module-sets-compile-time-seams.md)–[ADR-0050](../adr/0050-acl-pure-policy-cached-effective-ace-merge.md) and CONTEXT.md; when this record and those disagree, they win. Findings cite code as of `main` @ `6124b23`.

## Method

Six parallel read-only exploration passes over the four regions of the stack (both core planes independently double-covered; the duplicate passes converged, so confidence is high): L0–L3 substrate, L4 graph plane, net plane, bindings/integrations/conformance. Vocabulary: *module / interface / depth / shallow / seam / adapter / locality / deletion test* (a module earns its keep if deleting it would scatter complexity across callers). Grilling followed, one decision branch at a time; resolutions below.

## What is healthy (verified, leave alone)

- **`fwd_router_t`** — genuinely deep: capability-keyed (never identity-keyed; zero `dynamic_cast` in the tree), stateless, offset-only zero-heap forward dispatch; uniform across full-TLV and header-elided framing (load-bearing claim 4 holds in code).
- **`op_resolver_t`**, **`route_handle_t`** — pass the deletion test decisively.
- **`ws.hpp`** codec (socket-free, shared by both WS roles), **`crc.hpp`** (two-span feed), **`byteorder.hpp`**, **`opt_t`** — deep, single-locus primitives.
- **`can_link_t`** platform seam — clean link-time selection (`socketcan_link` vs stub vs esp-idf `twai_link`), zero in-source platform `#ifdef`.
- The conformance driver — one TAP contract, three native cores, no per-language branching (`harnesses.json` + `run-all.py` + `diff_fuzz.py`).
- Graph-plane clusters that **belong together for locality** (both passes agreed: do not split): fan-out + bubbling + idle-gate counters; the `:` field-write ioctl switch; storage + await.

## Findings → candidates → resolutions

| # | Finding (evidence) | Resolution |
|---|---|---|
| 1 | Canonical-key NAME-record walking open-coded ~6× in `graph.cpp` (`last_segment` :44, `parent_key` :59, `ensure_vertex` :290, `register_vertex_key` :256, `note_subscriber_*` :331/:343, `read_children` :968); `path_t` is a shallow validator | **key_view** navigation module in `tr::wire` beside `path_key` (parent / last NAME segment / is-ancestor-on-segment-boundary / segment iteration) |
| 2 | Four subscription-append paths, three bypass `field_write` (sugars `graph.cpp:672/:689`, wire `op_resolve.cpp:444`); duplicated SUBSCRIBER parse; remote-only durability latch; door-dependent ACL gate | **[ADR-0049](../adr/0049-field-write-single-subscriber-admission-door.md)** — `field_write` is the single admission door; uniform gate + latch (deliberate behavior change); resolves #59 |
| 3 | ACL evaluation inlined in `graph.cpp` anon namespace; promised `security_acl` seam absent; runs per gated data op with ancestor mutex-walk + clock read; "control-plane frequency" comment wrong (`graph.hpp:271`) | **[ADR-0050](../adr/0050-acl-pure-policy-cached-effective-ace-merge.md)** — pure per-target policy (ALLOW-only MCU / full-DENY host) + graph-side cached effective-ACE merge with generation invalidation |
| 4 | u32-length-prefix reassembly copy-pasted 3× (`transport_tcp.cpp:227` ≡ `transport_quic.cpp:197` ≡ `transport_webtransport.cpp:261`, self-labeled "verbatim"); POSIX recv-thread/teardown scaffold ~5×; ws-server/ws-client ~150 mutually duplicated lines; msquic scaffold cloned; `:settings` config walk 4×; DIAL/LISTEN factory validation 6× | **Extract**: `length_prefix_framer` (backend-fed), POSIX stream-endpoint base, shared `:settings` TLV reader, msquic endpoint base — all `tr::net` internals |
| 5 | ADR-0042 view delivery absent on exactly the doc-headline transports: WS double-copies egress + borrowed-span ingress (`transport_ws.cpp:109/:184`, admitted at `transport_vertex.cpp:167`); CAN builds the reassembly rope then `flatten()`s (`transport_can.cpp:361-392`) | **Zero-copy rework**: CAN delivers the rope with FD-padding trim = shortened last link; WS unmasks **in place** in the owned segment (RFC 6455 forces the byte-touch anyway) and composes egress as header-segment + payload links; client-side masking copy is protocol-mandated and documented as such |
| 6 | Same transport-seam contract re-proved 6× against bespoke socket setups; `fwd_multihop`/`fwd_compact` needlessly run over live WS (`fwd_multihop_test.cpp:183`); WS/base-CAN never exercised through the routing seam | **Parameterized transport-conformance harness** at the seam (a transport joining the target's set joins the suite — including the owning-tier row WS/CAN currently fail); router tests retarget to `loopback_channel_t` |
| 7 | Wire grammar forked (`frame.cpp:39-159` vs `tlv_arena.cpp:28-146`, equivalence by test only); header emission hand-rolled 4×; `kStructOptMask{0x48}` bypasses `opt_t`; both decoders span-only while the glossary promises rope-aware decode; `reference/08` §cast contradicts the code | **[ADR-0048](../adr/0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md)** — one grammar core behind a chunk-cursor (span/rope, ≤16 B straddled-header scratch); sinks unchanged; `compose`→rope emit dual; `reference/08` amended to the validating cast; no nested-rope type |
| 8 | `before_io`/`after_io`/`io_dir_t`: zero call sites; `alloc_hint_t` uninterpreted; `alignment()` uncalled; device copy off-seam as `cuda_copy_*` free functions; `std::function` type-erasure on the per-frame delivery path; ADR-0016 §3's cited ≤16 KB sentinel absent from CI | **[ADR-0047](../adr/0047-build-time-closed-module-sets-compile-time-seams.md)** — the appropriateness rule + L0 module set (concept, constexpr traits, tag-dispatched `transfer`), fn-ptr+context receivers, rope owning tier, `esp_heap_caps` first hint interpreter, arm-none-eabi sentinel; **net plane stays runtime-dispatched** (rule applied honestly) |
| 9 | L1 invariants comment-only: `over_bytes` conflates empty-input with backpressure (9 call sites disambiguate by hand); `subview` unchecked bounds; DEVICE-deref only warned; borrowed-span/`return_route` lifetime contracts in prose; `segment.space` denormalized | **Not yet grilled** (skipped in candidate selection) — see open items |
| 10 | Doc drift: `bindings/README.md:3` contradicts ADR-0028; HARNESS.md/coverage_audit "pending core" + "12 vectors" (actual 28); root README overclaims ros2 (a 2-function identity stub); stale "M3b" milestone headers; `0x0E SPEC` vector gap (#60); `mem_can_reassembly` mis-filed as a backend in reference/09; ADR-0032 perf matrix C++-only | **Docs batch PR**: fix all; ros2 claim reworded as roadmap |

## The doctrine (ADR-0047 §1, revised same-day)

Compile-time dispatch **only** where identity is per-target-fixed **and** the path is hot or size-critical; runtime dispatch where identity arrives as runtime data or the call is wiring-frequency; link-time source selection always closes the set regardless. Sorted for our seams:

- **Compile-time**: L0 backend set + tag fold, backend `constexpr` traits, parser chunk-cursor, ACL policy selection, rope small-buffer chain, fn-ptr+context receivers (code-size/no-heap rationale).
- **Runtime**: transport capability virtuals (wiring-frequency), factory-by-name (`:children[]` kinds are data), `transport_t::send` (syscall-dominated), segment tag dispatch across heterogeneous backends (dynamic per-object identity, minimized not eliminated).

## PR-train wave plan

- **Wave 0**: ADR-0047..0050 + CONTEXT.md + this record (PR #195) · arm-none-eabi ≤16 KB required-modules CI sentinel (baseline **before** churn) · docs-drift batch.
- **Wave 1**: `key_view` module · L0 module-set infrastructure + backend conversion (traits, `transfer`, `esp_heap_caps`).
- **Wave 2**: grammar + chunk-cursor parser (span + rope) + `compose` emit unification + `reference/08` amendment.
- **Wave 3**: receiver seam (fn-ptr + rope tier + rope SBO) → framer / POSIX / msquic extraction → WS/CAN zero-copy → transport-conformance harness. *(The earlier "transport set + trait fold" unit was dropped by the doctrine revision.)*
- **Wave 4**: admission door (ADR-0049) · ACL policy + cached merge (ADR-0050).

Dependencies: the sentinel precedes the template work it referees; rope-aware decode (W2) precedes rope delivery (W3); `key_view` (W1) precedes the ACL cache (W4).

## Round 2 (same day): the open items, grilled

All seven open items below were resolved in a second grilling round the same day:

| Item | Resolution |
|---|---|
| L1 contracts → types | `over_bytes` returns `std::optional<view_t>` (nullopt ⇔ alloc failure/backpressure; engaged-empty ⇔ empty input — nine call sites simplify); `subview`/`bytes()` bounds become **debug-build assertions** (zero release cost; fuzz+sanitizer CI catches violations); a `constexpr` backend trait **`owns_bytes`** (borrowed backends false) is asserted at durable-store doors (the ADR-0049 admission door, `store_ref_min_bytes` path) — a full owned/borrowed type split was rejected (L1 type fork); `segment_t::space` field dissolves into the module-set tag (space = per-backend trait, denormalization gone). Lands with Waves 1/3/4 respectively. |
| Error spine | `wire::error_t` **deleted** — the ADR-0048 grammar core returns `wire::err_t` directly (`err_path`/severity/disposition come free; conformance `ERR:<name>` strings unchanged); `graph::status_t` stays gated on RFC-0002 ratification as its header already states. Wave 2. |
| Namespace drift | `tlv_emit` folds into `tr::wire` with the grammar core (the upward-reaching `tr::detail` anomaly dies); `byteorder` and `crc` stay where they are (layer-free primitives with cross-layer consumers); `status.hpp` needs no move (already `tr::graph`) — only its doc grouping is fixed in the docs batch. Wave 2. |
| `mem_can_reassembly` | **Rehomed** to `tr::net::can_reassembly_t` beside `transport_can` (the self-admitted layer inversion dissolves instead of being documented); the two `std::map`s are replaced by storage from an injected `std::pmr::memory_resource` with bounded groups — exhaustion = evict-oldest + `dropped_groups` counter, never OOM. Keeps its module identity as the ADR-0011 totality/gap primitive. Wave 3. |
| 128-core scaling | **Measurement first**: contention benches join the perf matrix (`fanout_clone_storm` — one hot vertex, 8..128 subscribers, clone+release per delivery; `await_wakeup_storm`), nightly many-core row per ADR-0032. Three no-preclusion guards noted (all already true): backends instantiable per NUMA domain; `alloc_hint_t` backend-private; fan-out behind one internal function. No redesign without numbers. |
| Dispatch depth cap / limits | Escalated twice and landed at the principled end: **delivery terminates at the target** ([RFC-0007](../spec/rfcs/0007-delivery-terminates-at-target.md), [ADR-0051](../adr/0051-delivery-terminates-at-target-no-dispatch-limits.md)) — a SUBSCRIBER delivery applies target-local write effects and never re-dispatches to the target's `:subscribers[]`; propagation past a target is the target's logic (controller model). Dispatch cycles impossible by construction; `kMaxDispatchDepth` deleted with **no** replacement (an intermediate dedup+drain design was drafted and rejected as over-engineering). Wiring analysis = design-time reconciler/analyzer tooling. TLV nesting depth: fixed 32 removed ([RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md)) — receiver-resource-bounded, no conformance floor (protocol shapes nest ≤ 5 by construction); reference/01 + v1.md amended. `kMaxFrame` → per-connection `:settings`. The general rule is pinned as CONTEXT.md §Resource bound. |
| Bubbling/delivery order | **Unspecified by design** — reference note (one write delivers once per covered subscription; order unspecified; use `(origin, ts)` coherent sampling for ordering needs); tests assert delivery sets, never sequences. |
| TS `ws.ts` | Demoted to an explicit **`/codec` subpath export** with a role comment (interop oracle for `ws_diff_fuzz` + no-WebSocket runtimes; not the production framing path); bundles tree-shake it; the ADR-0028 drift-check oracle survives. |

**Wave-plan deltas from round 2**: Wave 2 additionally deletes `wire::error_t` and `kMaxDepth` (per RFC-0006, the parser bound becomes the injected arena; the conformance harness gains a declared decode budget) and moves `tlv_emit` into `tr::wire`. Wave 3 additionally rehomes CAN reassembly (pmr, bounded), moves `kMaxFrame` to `:settings`, and demotes `ws.ts`. Wave 4's graph work now implements RFC-0007 termination-at-target (simpler than the drafted dedup/drain — the `depth` parameter threads out entirely) plus the L1 durable-store asserts. New Wave 0e: contention benches + nightly many-core perf row.

## Open items — reviewed but not yet grilled (superseded — see Round 2 above)

1. **L1 contracts → types** (candidate 9, never selected): `over_bytes` two-outcome return, `subview` bounds, owned-vs-borrowed view distinction for `return_route`/arena spans, `segment.space` denormalization.
2. **Error spine**: three parallel vocabularies (`wire::error_t`, `wire::err_t`, `graph::status_t`) only partially bridged; namespace drift (`tlv_emit`/`byteorder` in `tr::detail`, `status.hpp` L4-in-codec, `tr::crc` outside the layer namespaces). Partially gated on RFC-0002 ratification.
3. **`mem_can_reassembly` home + MCU allocation discipline**: self-admitted `tr::mem`→`tr::view` layer inversion; two levels of `std::map` node allocation in a module that runs on P1 CAN leaves; the real reassembly decisions live in `transport_can` (4-file bounce).
4. **128-core scaling, second half**: the review targeted code structure; the many-core *data-plane* questions — segment refcount cacheline contention under wide fan-out, per-core/NUMA pool placement (an `alloc_hint_t` consumer), `await`/cv wakeup scaling — were named as a target but never designed. Needs perf data first (ADR-0032 matrix is C++-only and doesn't cover contention).
5. **Depth-cap silent truncation**: a fan-out chain beyond depth 32 is dropped while the originating `write()` reports success (`graph.cpp:461`) — interface honesty question (error? counter? documented-as-is?).
6. **Bubbling/delivery order**: nearest-ancestor-first, slot order within a vertex — implemented but unspecified and untested; decide whether to specify or pin "unspecified by design."
7. **TS `ws.ts` latent duplication**: a full RFC 6455 codec kept only as the cross-impl fuzzing oracle while production `TransportWs` delegates to the platform WebSocket — consolidate or annotate.
