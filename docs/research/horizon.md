# Research — the horizon: what libtracer is already positioned for, and the four gaps

> **Status:** research / design note (not published), dated 2026-08-19. Written to
> issue [#1385](https://github.com/avatarsd-llc/libtracer/issues/1385). **Descriptive,
> not normative.** It records where the tree actually stands and prices four things
> that are not built. It changes no spec text, opens no wire surface, and creates no
> obligation on anyone. Where it brushes against a ruling already on record, the
> tension is named in an admonition rather than quietly overridden.

## How to read this

The note has two halves and one rule.

**Part A** says where the project is already positioned for a decade in which more
software is written and operated by machines than by people. Every claim in Part A
names a file in this repository. A claim with no file behind it is not in Part A —
it is in the honesty section at the end of Part A, or it is not here at all.

**Part B** prices four gaps: WASM, post-quantum authentication, time-aware delivery,
and a formal wire grammar. Each gets the same three questions — *what is missing*,
*what is the smallest credible first step*, and *where does it attach* — plus a cost
comparison of doing it now versus in five years, and an explicit "what not to do yet".

**The rule:** this is a horizon note, not marketing. The strongest sentences in it are
the ones that say the project is not ready.

---

## Part A — where the project is already positioned

The bet underneath libtracer's process is that **machine-verifiable ground truth beats
prose**. An agent — or a stranger, or the maintainer in three years — should be able to
ask "is this implementation correct?" and get an answer from a program rather than from
a paragraph. Six mechanisms in the tree implement that bet.

### A.1 — The compatibility test is a directory of bytes, not an opinion

`docs/spec/v1.md` §4 defines conformance in two clauses: honour every MUST, and pass
every vector under `tests/conformance/vectors/v1/`. It then closes the door in one
sentence — *"There is no other compatibility test."* The same section pins the layout
(`vectors/v1/<category>/<case>/`), the round-trip obligation
(`encode(decode(x)) == x`, byte-exact), and the governance edge that matters most:
**adding a vector is not a spec change; changing an existing vector's bytes is.**

What is actually there, counted in the tree today:

| | count |
| --- | ---: |
| categories under `tests/conformance/vectors/v1/` | 14 (`acl`, `crc`, `errors`, `field`, `framing`, `fwd`, `path`, `path-label`, `path-ref`, `settings`, `spec`, `stream`, `subscriber`, `tlv-types`) |
| case directories (each with an `expected.json`) | 92 |
| positive cases (`input.bin`, must round-trip) | 85 |
| negative cases (`reject.bin`, must be refused) | 7 |

The negative cases are the load-bearing ones. A vector suite that only proves what an
implementation *accepts* certifies a permissive decoder as conformant; the `reject.bin`
cases are what make the boundary of the format testable rather than describable.

Around the vectors sit `tests/conformance/run-all.py` (the cross-core driver),
`tests/conformance/diff_fuzz.py` and `tests/conformance/ws_diff_fuzz.py` (differential
fuzzers that generate frames the vectors never thought of and require all cores to
agree), `tests/conformance/coverage_audit.py`, and two documents that tell an outside
implementer how to plug in — `tests/conformance/README.md` and
`tests/conformance/HARNESS.md`. The harness list itself is data, not code:
`tests/conformance/harnesses.json` declares three enabled harnesses (`cpp`, `ts`,
`rust`) with their invocation lines. Adding a fourth implementation is a JSON entry
plus a binary that speaks TAP — which is exactly the shape gap B.1 needs.

`.github/workflows/conformance.yml` runs the driver and the differential fuzzer
(`diff_fuzz.py -n 2000`) on every relevant change.

### A.2 — Three independent cores, held together by the same bytes

`docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md` rules that each
language gets a **native reimplementation**, explicitly rejecting the alternative of one
C core reached by FFI and WASM-of-C. That decision buys idiomatic, dependency-free cores
and pays for it in drift risk — and it names the drift guard in the same breath: the
shared vectors.

The three cores exist:

- **C++** — `core/`, the golden reference. Its wire grammar is consolidated in
  `core/include/libtracer/grammar.hpp` per
  `docs/adr/0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md`.
- **Rust** — `bindings/rust/`. `bindings/rust/Cargo.toml` is sixteen lines long and has
  **no `[dependencies]` section at all**; `bindings/rust/src/lib.rs` opens `#![no_std]`
  (it needs `alloc` only for the owned TLV tree). It carries both
  `bindings/rust/examples/conformance.rs` (the vector harness) and
  `bindings/rust/tests/conformance_vectors.rs` (in-crate structural tests).
- **TypeScript** — `bindings/typescript/packages/`, split into `core`, `client`,
  `transport-ws` and `transport-webtransport`, with the vector harness at
  `bindings/typescript/packages/core/conformance/harness.mjs`.

`docs/adr/0032-continuous-cross-core-perf-conformance-matrix.md` makes the cross-core
comparison continuous rather than occasional. The property this buys is unusual and
worth naming plainly: **three unrelated codebases, written in three languages with three
memory models, are required to agree byte-for-byte on the same corpus, continuously.**
That is a much stronger statement about the format than any one implementation passing
its own tests, and it is the single most transferable asset the project has.

### A.3 — Governance is written down, and the instruments are typed

`.github/GOVERNANCE.md` splits decisions into three domains (spec / reference
implementation / tooling) with different rules for each, and — more usefully — types the
two instruments a normative document can be changed with:

- an **erratum** is for text that contradicts already-shipped, already-agreed behaviour;
  ordinary PR, no comment window, and *may not alter the wire surface* — if applying it
  would change what a conforming implementation does, it is not an erratum;
- an **amendment** is for a change to the normative surface itself; RFC plus maintainer
  approval, with the 14-day comment window **waived by default** while the project is
  solo-maintained and invoked explicitly when outside input is wanted.

The waiver is argued in the document rather than asserted, and the argument is honest
about its cost. The tree behind the process is real: 82 numbered ADRs under `docs/adr/`
and 26 RFC documents under `docs/spec/rfcs/`.

### A.4 — The docs are gated against the tree

`tools/check_doc_citations.py` pins each `file:line` citation in the docs to a substring
the cited line must contain, so a doc that points at code fails CI when the code moves
out from under it. `.github/workflows/doc-citations.yml` runs it — and runs the
resolver's own unit tests first, so a resolver bug reports as a resolver bug instead of
as three hundred phantom drifts.

`tools/gen_capability_matrix.py` goes one step further: `docs/capability-matrix.md` is
*generated*, and `--check` (wired in `.github/workflows/capability-matrix.yml`) fails
when a ✅ on that page is not backed by an artifact CI actually runs, or when the
committed page has gone stale. A capability claim in this project is a claim about a
test, not about an intention.

`tools/wireshark/libtracer.lua` is decoded against every conformance vector by
`.github/workflows/wireshark-dissector.yml`, so even the packet dissector is held to the
same corpus.

### A.5 — Size and shape are ratcheted, and the ratchets state their own limits

`bench/symbol_ratchet.py` + `bench/symbol_ratchet.json` pin the sizes of the hot dispatch
symbols. The design is careful in a way worth copying: it is **toolchain-bound** (it
refuses to compare across compilers rather than report a difference it cannot attribute),
it is a **ratchet, not a ceiling** (a shrink fails too, with a re-pin instruction, so a
pin can never sit above the truth), and its own docstring insists it measures *code
shape, not time* — "read a failure as *something moved, go price it*, never as *this is
slower*."

`tools/cortexm0_footprint.py` cross-compiles a fixed minimum-feature node
(`core/tests/footprint/sentinel_node.cpp`) for `arm-none-eabi` Cortex-M0 at `-Os` and
sizes it, via `.github/workflows/footprint-cortexm0.yml`.

### A.6 — Heterogeneous memory is in the model, not bolted on

`core/include/libtracer/backend.hpp` carries `io_dir_t` (`CPU_TO_DEVICE` /
`DEVICE_TO_CPU`), the `before_io` / `after_io` cache hooks, and a `transfer()` seam;
`core/include/libtracer/mem_cuda.hpp` is a real backend behind it.
`docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md` rules that framing stays
host-side while a TLV becomes a **heterogeneous host+device rope** — a single logical
value whose segments live in different memory spaces —, and
`docs/adr/0039-pmr-memory-model-host-aligned-allocation.md` unifies the allocation story.
`docs/reference/00-overview.md` §"The six-layer model" puts the memory substrate at L0
with an explicit `alloc` / `release` / cache-hook interface up to L1.

The consequence: "the payload is on a GPU" is an ordinary case for this protocol rather
than an integration project. For a decade in which most interesting payloads are
produced or consumed by an accelerator, that is the right place to have already been.

```{admonition} What Part A does NOT claim
:class: warning
Four things get overstated when this material is summarised. They are stated correctly
here so the summary cannot.

**The spec is a DRAFT.** `docs/spec/v1.md` line 1 says so, and the second line says the
wire format is not yet stable and that consumers should pin to a commit. §5 says v1 is
immutable *once finalized*. It is not finalized. Every stability argument in Part A is an
argument about *process*, not about a frozen wire.

**The Cortex-M0 footprint gate is a sentinel, not a budget.**
`.github/workflows/footprint-cortexm0.yml` invokes the tool with `--mode warn` and
`--drift-mode warn`. `tools/cortexm0_footprint.py`'s own docstring gives the reason: the
measured node "has been over the 16 KiB bound continuously, so a hard gate would red
main", and the standing overage is explicitly **not attributed** today. The drift half is
also `warn`. So: over budget, by an unexplained amount, and nothing fails.

**A second footprint tool used to be wired into nothing** — `tools/rv32_footprint.py`
existed, was used by hand for RFC-0018's falsifier 6, and was referenced by no workflow,
which made it a script rather than an instrument. It now runs on every ESP-plane change
as the `rv32-census` job in `.github/workflows/esp-idf.yml`
([#1479](https://github.com/avatarsd-llc/libtracer/issues/1479)). Read the correction at its own
width: the job is **advisory** by ruling and stays so — it publishes per-section and
per-translation-unit bytes as an artifact, and it sets no ceiling (the footprint contract
in that file's header) and no drift ratchet (no run-to-run noise band has been measured
yet to set one against). The gap that closed is "nothing runs it"; the gap that remains
is that nothing on the rv32 side *fails*.

**The implementer registry is empty.** `docs/implementations.md` lists `_(none yet)_`.
Its closing paragraph says listing gives implementers "a standing seat in spec
discussions" — a governance provision with, at this date, zero occupants. Three
*first-party* cores cross-validating is a strong claim; an *independent* implementation
passing the vectors would be a categorically different one, and has not happened.
```

---

## Part B — the four gaps

Common structure: what is missing, the smallest credible first step, where it attaches,
now-versus-later cost, and what not to do yet.

### B.1 — WASM: no native core compiles to wasm32

**What is missing.** A grep for `wasm32`, `wasi`, `emscripten`, `wasmtime`,
`wasm-pack` or `wasm-bindgen` across every workflow, CMake file, Cargo manifest and JSON
manifest in this repository returns **nothing**. The browser is served by the TypeScript
core alone. Note the asymmetry with what the catalog already contemplates:
`docs/reference/10-module-catalog.md` lists an `executor_wasm` (WAMR) slot as post-MVP —
running *someone else's* WASM inside a node — and a `transport_ws` described as "browser
and WASM reachable". The direction that does not exist is the other one: **libtracer
itself as a WASM artifact.**

The Rust core is closer to it than anything else in the tree, and says so: the header
comment in `bindings/rust/src/lib.rs` states it compiles `#![no_std]` "so it can target
WASM and bare-metal MCUs". That claim is currently untested by CI.

**Smallest credible first step.** Build the Rust codec for `wasm32-wasip1` and run
`bindings/rust/examples/conformance.rs` over the *existing* vectors under `wasmtime`, as
a **fourth execution environment** rather than a fourth core. No new code, no API design,
no published artifact: a toolchain target, a runner, and one more row on the ADR-0032
matrix. Because the harness list is data
(`tests/conformance/harnesses.json`), this is close to a JSON entry plus a CI job.

**Where it attaches.** Issue [#1386](https://github.com/avatarsd-llc/libtracer/issues/1386)
(`ready-for-human` — the scope decisions in it are genuine rulings, not paperwork:
which core, codec-only versus full graph, CI shape, and whether anything is published).
ADR-0032 is the matrix it extends.

```{admonition} Tension with ADR-0028 — named, not overridden
:class: important
`docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md` explicitly rejects
option B, "one C core + FFI/WASM bindings", on the grounds that it "ships a heavy WASM
bundle to the browser (where the client only needs a small codec + WebSocket)".

That rejection is about **how the browser is served**, and this gap does not reopen it.
The browser stays pure-TS. What B.1 proposes is a *different target* — edge functions,
plugin sandboxes, component-model hosts — reached by compiling the already-native Rust
core, not by wrapping C. Nothing here proposes a WASM bundle in the browser, and nothing
here proposes FFI.

The honest caveat: if a wasm32 artifact ever became the *recommended* browser path, that
**would** be a reversal of ADR-0028 and would need its own ADR. Whoever picks up #1386
should keep those two futures separate on purpose.
```

**Now vs in five years.** Cheap now, and the cost does not obviously grow — this is the
mildest of the four gaps, and the argument for doing it early is signal, not risk: a
fourth substrate exercising the same corpus catches endianness, alignment and
`usize`-width assumptions that three 64-bit hosts never will.

**What not to do yet.** Do not publish a WASM artifact to a registry, do not attempt the
full graph (threads and sockets in a WASM host is a design problem, not a build problem),
and do not touch the C++ core via wasi-sdk until the Rust route has proven the CI shape.

### B.2 — Post-quantum authentication: the requirement is not written down

**What is missing.** `docs/adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md`
sets the direction: authentication is ordinary graph reads and writes over the existing
pluggable-subject seam — `subject_token_t` plus the resolver the ADR calls
`subject_resolver_t` and the header now spells `subject_resolver_fn_t`, both in
`core/include/libtracer/graph.hpp`; **core takes zero crypto**; identity is
per-hop raw ed25519 with trust-on-first-use; link confidentiality comes from TLS 1.3 on
QUIC/WebTransport or, for plaintext MCU links, a Noise-pattern channel in the catalog's
`security_noise` slot. X.509 and end-to-end multi-hop identity are rejected outright.

Searched across the whole tree, the words *post-quantum*, *ML-KEM*, *Kyber*, *Dilithium*
and *PQC* appear **exactly once**, in
`docs/spec/rfcs/0011-node-identity-facet.md`. That single occurrence is the good news:
the identity record is `{kind, key}` rather than a bare 32-byte pubkey, and the RFC says
why in as many words — the `kind` member is "24 bytes of overhead per read … that buys a
post-quantum or second-kind future without a surface break", with `0x01` assigned to
ed25519 and a rule that a `kind` outside the registry MUST be rejected.

So the *identity format* is already extensible. What is not written down is the
**session handshake** requirement, and that is the half where harvest-now-decrypt-later
bites: a recorded Noise session with a classical-only KEM is decryptable later, whereas a
signature verified today is not retroactively forgeable.

**Smallest credible first step.** A dated constraints note — a paragraph, not a design —
recorded **before** the handshake is specced, saying three things: (1) the session
handshake MUST be hybrid-capable, classical X25519 combined with a PQ KEM in the
established hybrid pattern; (2) identity keys MAY stay classical, because the `kind` tag
already makes rotation a value change rather than a format break; (3) downgrade and
negotiation rules between a PQ-capable node and a classical-only MCU peer must be
written, with the NARROW-target RAM cost stated as a measured number. Reserving a `kind`
registry value for a hybrid suite is the concrete artifact.

**Where it attaches.** Issue [#1387](https://github.com/avatarsd-llc/libtracer/issues/1387)
(`ready-for-agent`), which names `docs/research/` as the home for that note and requires
it to be folded into the auth ADR/RFC when that is written. ADR-0045 is what it
constrains; RFC-0011's `kind` registry is where it lands on the wire.

```{admonition} Numbers this note deliberately does not state
:class: warning
ML-KEM parameter sizes are external facts from FIPS 203, not measurements from this tree,
and **nothing in this repository has measured them on an ESP32-class node** — there is no
crypto in core to measure. This note therefore quotes no key, ciphertext or RAM figure.

That is not an oversight: issue #1387's constraint 3 makes "what the hybrid handshake
costs on an ESP32-class node" its own deliverable, and a horizon note that guessed the
number would give the follow-up a fake baseline to check itself against. The NARROW-target
verdict — whether a hybrid handshake fits at all on 16 KB-class hardware — belongs to
#1387 and is genuinely open here.
```

**Now vs in five years.** This is the widest now-versus-later spread of the four. Today
it is a paragraph and a reserved registry value, because the handshake does not exist
yet and the identity record already has its escape hatch. Once a Noise handshake ships
and peers TOFU-pin each other across a fleet, changing the suite is a **wire break** in a
project whose whole stance is that the wire is immutable once released — plus a fleet-wide
re-pairing. The asymmetry is the entire argument.

**What not to do yet.** Do not implement anything. Do not put crypto in core — ADR-0045
decision 5 rejects that and nothing here disturbs it. Do not specify the handshake in
order to make it hybrid; the requirement is recorded first precisely so the handshake can
be designed once, later, with the constraint already known.

### B.3 — Time-aware delivery: the weakest position of the four

**What is missing.** Say this plainly: **libtracer cannot express a deadline, and its
timestamps are not comparable across producers.** Both halves are deliberate, and each is
on record.

*The knob was deleted, not deferred.* RFC-0022
(`docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md`) §2
found `deadline_ns` **inert** — a name in a knob map, an assignment, and an `emit_value`,
with "no comparison, no arithmetic, no branch" anywhere in `core/src` or `core/include`.
A client could write it, read it back, see it in `:schema`, and nothing would ever honour
it. §E removed it, arguing that moving a dead field is worse than deleting it.

*The replacement carries flags only.* RFC-0022 §3.A packs a 16-bit `delivery_policy` on
the **subscription**: bits 0–1 `reliability`, 2–4 `priority`, bit 5
`durability_request`, bits 6–15 reserved-MUST-be-zero. §A states the rule directly — "no
magnitudes are packed", because a bit-width on a magnitude is a synthetic limit this
project forbids. `core/include/libtracer/subscriber.hpp` repeats it at the point of
definition: "A deadline or a queue bound added later is a magnitude and belongs in the
subscription's cold half as a full-width field, never in these bits." The same header is
candid that `reliability` and `priority` are today **stored and read back, awaiting the
transport work that honours them**.

*The ROS hole is documented as a hole.* `bindings/ros2/README.md` §QoS maps `deadline`,
`liveliness` and `lifespan` to "**no mapping at all**", and says so in the R3 phase row
too. That is the honest state, but it is a hole a robotics integrator will walk into.

*And there is no shared clock, by design.*
`docs/adr/0019-per-producer-monotonic-origin-timestamp.md` makes `origin_timestamp` a
per-producer hybrid logical clock and rules that **cross-producer ordering is undefined
by design** — two producers' timestamps are never comparable; wall-clock interpretation
is advisory; coherent sampling across origins "requires a coordinated trigger or external
clock sync … never obtained by comparing `ts` across origins."

This is why the gap is deeper than a missing field. A deadline on a subscription is a
field. **TSN-class determinism needs a shared time base, which ADR-0019 explicitly
declines to provide.** Those are two different asks and should never be sold as one.

**Smallest credible first step.** One RFC that answers the ROS QoS hole and a deadline
field *together* — the issue's "one design should answer both" — scoped to the *field*
question only: a full-width deadline in the subscription's cold half, exactly where
`subscriber.hpp` says such a magnitude belongs, with a stated behaviour on expiry (drop?
mark? deliver-late-with-a-flag?) and a stated mapping for ROS `deadline` / `lifespan`.
`liveliness` is a different animal and should be split off rather than smuggled in.
Deciding whether stored-but-uninterpreted `priority` gains an interpretation belongs in
the same RFC, since a deadline without priority arbitration is half a scheduler.

**Where it attaches.** RFC-0022 (it owns the delivery-policy surface and deleted the old
knob) and `docs/adr/0023-ros2-binding-via-rmw-tracer.md` plus `bindings/ros2/README.md`
(they own the unmapped QoS rows). ADR-0019 is the boundary the RFC must **not** cross
without its own decision.

```{admonition} Tension with ADR-0019 and RFC-0022 — the shape a proposal must respect
:class: important
Two rulings constrain any time-aware work, and a proposal that ignores either is not a
proposal, it is a reversal.

**RFC-0022 §3.A / §3.E:** no magnitudes in the packed policy bits, and a deleted knob is
not re-added by moving it. A deadline is a full-width field in the cold half or it is
nothing. Spending reserved bits 6–15 on a duration would contradict §A directly.

**ADR-0019:** cross-producer ordering is undefined by design. Anything marketed as
"TSN-class" or "globally ordered" requires a shared time base that this project has
declined to require, and adopting one is an ADR-level reversal — not an RFC detail. A
*per-subscription* deadline, measured against the receiving node's own clock, needs no
such reversal. Keep the two separate: the first is buildable, the second is a different
project.
```

**Now vs in five years.** Cheap now as a *design*, expensive later as a *retrofit*, but
the honest framing is different from B.2: nothing is shipping today that a deadline would
break, so the cost of waiting is market-facing rather than technical. Industrial and
robotics buyers ask this question early, and "no mapping at all" in a README is a worse
answer than "here is the RFC and here is what it deliberately does not promise".

**What not to do yet.** Do not add a deadline to the packed bits. Do not promise
determinism, TSN, or cross-node scheduling of any kind. Do not adopt a global clock to
make a deadline work — a receiver-local deadline does not need one, and reaching for a
shared time base to solve a per-subscription problem would be the expensive mistake here.

### B.4 — Formal wire grammar: an implementation grammar, not a machine-readable one

**What is missing.** The format is described three times in the tree, and none of the
three is a machine-readable grammar:

1. **An implementation grammar.** `core/include/libtracer/grammar.hpp` is, per
   `docs/adr/0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md`, "the one
   wire-grammar core" — type-`0x00` reject, reserved-bit reject, `LL` length width,
   trailer sizing, the two-span CRC, parsed and validated in one place behind a
   chunk-cursor. It is excellent, and it is C++: an implementer in another language must
   *read* it, not *consume* it.
2. **A prose grammar for paths only.** `docs/reference/03-addressing.md` gives EBNF in
   ABNF-like notation — for the path syntax. It covers no other part of the frame.
3. **A hand-maintained second model.** `tools/wireshark/libtracer.lua` is 832 lines of
   Lua that re-describe the same format for a dissector, kept in lockstep by hand and
   checked against the vectors by `.github/workflows/wireshark-dissector.yml`.

The vectors prove **points** in the space — 92 of them, thoroughly. A grammar proves
things about **the space**: that a given byte string is or is not well-formed, without
anyone having to have thought of it first. That is also the artifact an agent or an
external tool can consume directly, which is why it compounds with everything in Part A.

**Smallest credible first step.** A machine-readable, Kaitai-class grammar covering the
frame header, the TLV structure, and the packed-PATH body (RFC-0018's `[u8 len][utf8]`
segment records) — validated as a **fifth checker** against every existing `input.bin`
(must parse) and every `reject.bin` (must fail), and against nothing else. Descriptive
only; a tooling artifact, in `tools/`, with no claim on conformance. If it disagrees with
the C++ core, the C++ core wins and the grammar is wrong.

**Where it attaches.** ADR-0048 (it owns the grammar consolidation and is the natural
place to note a descriptive companion), `docs/reference/01-data-format.md` and
`docs/reference/05-protocol-tlvs.md` (the prose the grammar would mirror), and the
existing vector corpus as its test set. **No issue is filed for this** — see the
decisions section below.

```{admonition} Tension with the spec — a grammar must stay descriptive
:class: important
`docs/spec/v1.md` §4 says conformance is the MUST clauses plus the vectors, and closes
with "There is no other compatibility test."

A grammar that stayed in `tools/` and was checked against the vectors is a **tooling**
artifact: it adds no compatibility test, it only re-derives one. That needs no RFC.

Making the grammar **normative** — "an implementation is conformant if it accepts exactly
the language this grammar defines" — would change §4, which is a change to the normative
surface and therefore an **amendment** under `.github/GOVERNANCE.md`: RFC plus maintainer
approval. It would also be a strictly stronger claim than the project can currently
back, since a grammar can only describe the structural layer and several v1 rules
(CRC, canonical-context restrictions, ACL behaviour) are not structural. Build the
descriptive one; do not let it drift into the spec by accident.
```

**Now vs in five years.** Roughly linear in the size of the format, so the cost grows
with the format rather than with the calendar — which makes this the least urgent of the
four and the easiest to keep deferring. The counter-argument is B.4's second-order value:
a machine-readable grammar is the input to fuzzers, to third-party dissectors, and to any
agent trying to implement the protocol without reading 832 lines of Lua.

**What not to do yet.** Do not make it normative. Do not attempt to express CRC,
canonical-context rules or ACL semantics in it — the grammar covers structure, and
pretending otherwise produces a document that is wrong in the interesting cases. Do not
retire the Lua dissector's model on the strength of it.

---

## The four gaps side by side

| Gap | Cost now | Cost in five years | Attaches to |
| --- | --- | --- | --- |
| **B.1 WASM** | a toolchain target + one CI job; no API design | similar — grows only if a wasm path became the recommended browser route (an ADR-0028 reversal) | [#1386](https://github.com/avatarsd-llc/libtracer/issues/1386), ADR-0032 |
| **B.2 Post-quantum auth** | a paragraph + a reserved `kind` value | a **wire break** plus fleet-wide re-pairing, in a project whose wire is immutable once released | [#1387](https://github.com/avatarsd-llc/libtracer/issues/1387), ADR-0045, RFC-0011 |
| **B.3 Time-aware delivery** | one RFC; a cold-half field and a ROS mapping | a retrofit into shipped subscriptions — and the deeper half (shared time base) is an ADR-0019 reversal at any date | RFC-0022, ADR-0023, `bindings/ros2/README.md` |
| **B.4 Formal wire grammar** | one descriptive grammar + a vector-corpus check | scales with the format, not the calendar — the least urgent | ADR-0048, reference 01/05, the vectors |

Ranked by the now-versus-later asymmetry alone, the order to act is **B.2, B.1, B.3,
B.4** — B.2 because it is the only one whose later cost is a wire break, B.1 because it
is nearly free and buys a genuinely new substrate on the existing corpus, and B.3 before
B.4 because a buyer asks about deadlines and nobody asks about grammars.

---

## Decisions taken in writing this note

Recorded so the next reader does not have to reconstruct them.

1. **No new issues were filed for gaps B.3 and B.4.** Triage filed concrete follow-ups
   for WASM (#1386) and post-quantum (#1387) only. For the other two this note names the
   first step and its attachment point instead; filing speculative issues against ADRs
   that would have to be revisited anyway would add tracker noise, not clarity.
2. **The filename is `docs/research/horizon.md`**, as pinned by #1385, even though two
   siblings in this directory carry a `YYYY-MM-DD-` prefix. The date lives in the status
   line above.
3. **Nothing outside this file changed.** No code, no `docs/spec/`, no ADR, no RFC. Every
   place this note brushes a ruling, it is an admonition naming the tension — B.1 against
   ADR-0028, B.3 against ADR-0019 and RFC-0022, B.4 against `docs/spec/v1.md` §4 — and
   never an edit.
4. **No number in this note is imported from outside the tree.** The ML-KEM sizes that
   would ordinarily appear in B.2 are absent for the reason given there.
