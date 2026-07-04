# Delivery terminates at the target: no re-dispatch, no dispatch-depth cap, no termination machinery — propagation past a target is the target's logic

Status: accepted. Supersedes the in-process dispatch-depth cap of [ADR-0015](0015-graph-runtime-concurrency-and-in-process-cycle-cap.md) (its concurrency model is untouched); implementation-side of [RFC-0007](../spec/rfcs/0007-delivery-terminates-at-target.md); applies the no-synthetic-limits principle of [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md) to the graph runtime; maintainer-ratified 2026-07-04.

## Context

`kMaxDispatchDepth = 32` existed because delivery was implemented as a *recursive write*: `fan_out` re-entered `write_impl` on each SUBSCRIBER target (`graph.cpp:466`), so a subscription cycle recursed forever without a cap, and the cap silently truncated deep legitimate chains while the originating `write()` reported success. A first redesign (chain dedup + drain queue) managed the cascade; the maintainer cut deeper: **the cascade itself is wrong.** The runtime must not police user designs with synthetic limits — and it doesn't need to, because automatic transitive relay was never the intended semantics of the wiring model (edges deliver producer → consumer; *controllers* mediate propagation).

## Decision

### 1. A SUBSCRIBER delivery terminates at its target

Delivery into a target T performs exactly the target-local effects of a write — store (LKV/history per T's role), T's write-ACL gate, `await` wake, T's local reaction (handler/callback) — and **never re-dispatches to T's own `:subscribers[]`**. Whether, when, and with what value T's subscribers hear anything is exclusively the decision of **the logic behind T**: a controller reads its input ports and writes its output ports on its own execution; a handler re-emits when it chooses; a plain stored-value vertex simply holds the value. An application wanting pure relay subscribes the final consumer directly to the source (subtree subscriptions make this cheap) — it does not chain plain vertices.

Propagation from one `write()` is therefore **one hop plus upward bubbling** ([RFC-0005](../spec/rfcs/0005-subtree-subscriptions.md) — strictly toward the root, bounded by tree height, cannot loop). Dispatch-level cycles are impossible **by construction**.

### 2. `kMaxDispatchDepth` is deleted — and nothing replaces it

No cap, no per-chain dedup, no drain queue: the recursion those mechanisms bounded no longer exists, and fan-out is a single-level iterative loop with O(1) native stack. The only code path where user logic can still recurse is a subscriber **callback that itself calls `write()`** — a new first-hop write from user code, the same class as infinite recursion in any program: the user's responsibility, not the runtime's to police.

### 3. Wiring analysis is design-time tooling, never runtime enforcement

Detecting suspicious topologies — subscription cycles among controller ports, undamped feedback loops, dead chains (a plain vertex in the middle of an intended relay) — belongs to the **analyzer/reconciler layer** (the manifest reconciler and orchestrator tooling of reference/13, a graph linter over `read(':subscribers[]')` state). It warns the user while they design; the runtime executes what was wired. Recorded as tooling roadmap.

### 4. Order and remaining constants (unchanged from the ratified round)

Delivery order within one write stays **unspecified by design** (reference note; tests assert delivery sets, never sequences). Transport `kMaxFrame` becomes per-connection `:settings` with a per-target default; tuning knobs that are not limits (rope inline links) are exempt.

## Considered options

- **Keep the cap (+ observability counter).** Rejected: synthetic at any value; silently truncates legitimate chains; polices the user.
- **Chain dedup + iterative drain (the first draft of this ADR).** Rejected as over-engineering once termination-at-target is adopted: it manages a cascade that should not exist. Its one residual virtue (making callback-write cycles livelock observably instead of recursing the native stack) does not justify per-write queue machinery for a user-bug class.
- **Terminate-at-target but queue callback deliveries.** Rejected for the same reason — machinery whose only beneficiary is broken user code.
- **Auto-relay as an opt-in edge flag.** Rejected: reintroduces the cascade and its termination problem behind a flag; explicit re-emission by target logic composes better and matches the controller model.

## Consequences

- `graph.cpp` `fan_out` leg (b) stops calling `write_impl` recursively; delivery becomes `store + notify` on the target. The `depth` parameter threads out of the code entirely.
- **Behavior change** (interop-visible, hence RFC-0007): chained plain-vertex subscriptions (`A→B`, `B→C`) no longer relay `A`'s writes to `C`. Migration: subscribe `C` to `A` directly, or make `B` a handler that re-emits.
- CONTEXT.md §SUBSCRIBER gains the termination sentence (the "indistinguishable from a direct write" claim is target-perspective and survives); §Cycle termination's in-process half is rewritten (loop-free by construction, matching the FWD plane's story).
- ADR-0015 gains a superseded-in-part annotation; depth-cap tests are replaced by termination tests (a chained pair does NOT relay; a controller re-emit DOES reach its port's subscribers) and set-based order tests.
- The graph interface sheds an invisible contract: callers no longer need to know a truncation depth exists (finding "silent depth-cap truncation" resolves by the limit's nonexistence).
