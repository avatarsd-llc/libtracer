# Architecture-deepening — implementation progress & handoff (2026-07-04)

> **Status**: session record (descriptive). The *plan* is
> [2026-07-04-architecture-deepening-review.md](2026-07-04-architecture-deepening-review.md);
> the *decisions* are the ADRs/RFCs it cites. This doc records what has been
> **implemented and merged**, the working conventions discovered along the way,
> and the concrete design for each **remaining** unit — enough for a fresh session
> to resume precisely. When this record and the ADRs disagree, the ADRs win.

## 1. What has landed (merged to `main`, all green)

Nine PRs, landed in one autonomous solo-maintainer session (branch → PR → CI →
`--admin` merge). `main` @ `94d49b3`, zero open PRs, 27/27 core tests green.

| PR | Wave | What it did | Validation |
| --- | --- | --- | --- |
| #199 | 0b | **Cortex-M0 ≤16 KB footprint sentinel** — `tools/cortexm0_footprint.py` + `core/tests/footprint/sentinel_node.cpp` + `.github/workflows/footprint-cortexm0.yml`. **Warn-mode**: the P0 node measures ~16.9 KiB, ~0.9 KiB over 16 KiB (`std::pmr` soft-float via the arena decoder ~2.7 KiB + CRC tables ~1.5 KiB). The referee for the ADR-0047 compile-time doctrine. | built + measured w/ arm-none-eabi 14.3 |
| #200 | 0c | **Docs-drift batch** — bindings/README vs ADR-0028 (native cores, not wrappers); ROS 2 GPU overclaim → roadmap; stale conformance-harness core counts (2→3, de-numbered "12"); `scripts/`→`tools/` straggler in a `.c`. | — |
| #201 | 0e | **Many-core contention benches** — `bench_fanout_clone_storm` (segment-refcount cacheline contention: aggregate saturates ~30–56 M ops/s, no core scaling), `bench_await_wakeup_storm` (await/cv fan-in: write cost 0.47→26 µs at 1→128 waiters). Diagnostic, NOT in the perf gate. | TSan-clean |
| #202 | 1 | **`tr::wire::key_view_t`** (`key_view.hpp`) — extracted the canonical-key NAME navigation open-coded ~7× in `graph.cpp` (last-segment, parent, ancestor/child, level-split). Behavior-preserving. | 27 tests + ASan + Doxygen + a dedicated `key_view_test` |
| #203 | 3 | **Demoted the TS RFC 6455 codec to the `./ws` subpath** — removed the barrel re-export so it tree-shakes out of `TransportWs`-only bundles. | `npm test` green |
| #204 | — | Removed a stray `__pycache__` pyc that #200 accidentally committed. | — |
| #205 | 2 | **`tlv_emit` → `tr::wire`** — moved `emit_tlv`/`emit_name` out of the layer-free `tr::detail` (they produce wire bytes from wire types). ~246 call sites swept. | 27 tests incl. conformance (byte-exact) |
| #206 | — | **L1 bounds asserts** — debug-build `assert`s on `view_t::subview`/`bytes()`; the sentinel now passes `-DNDEBUG` (release profile). Zero release cost. | 27 tests w/ asserts ACTIVE under ASan |
| #207 | 1 | **ADR-0047 §2 — build-time-closed backend set + tag dispatch** for segment release (see §3.1). The substrate seam. | 27 tests host + ASan+UBSan + TSan + esp-idf + sentinel |

## 2. Working conventions (discovered this session — read before resuming)

- **Every change via a signed PR.** `git commit -s` (DCO). **No `Co-Authored-By`
  trailers.** No direct pushes to `main`.
- **Merging agent-authored PRs needs an explicit `--admin` authorization.** The
  Claude Code auto-mode classifier blocks `gh pr merge` on the agent's own
  session PRs as "Self-Approval"; a general "merge automatically" is **not**
  enough — the maintainer must say **"use admin"** (or add a Bash permission
  rule). Then: `gh pr merge <N> --merge --admin --delete-branch`. `--auto` is
  permitted but stays `BLOCKED` on the required review. Squash is disabled
  repo-side (`allow_squash_merge:false`) — use `--merge`.
- **clang-format version skew.** Local is 14; **CI is 18**. They diverge (seen:
  `::operator delete (` — 14 adds a space, 18 rejects it). Hand-fix from the CI
  log, or `pip install clang-format` (ships 18+). Do not trust local 14 on
  `operator new/delete`.
- **Validation gauntlet for any `core/` change** (run locally before pushing,
  wait for CI green before an `--admin` merge):
  `cmake --build core/build` + `ctest` (27) · ASan+UBSan (`-fsanitize=address,undefined`)
  · TSan on the threaded refcount tests (`substrate|graph|fwd_fanout|subtree`)
  · conformance vectors · `python3 tools/cortexm0_footprint.py` (arm-none-eabi at
  `~/.local/share/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin`).
- **Any target that lists core `src/*.cpp` explicitly must include new core
  files.** Not just `core/CMakeLists.txt` — also
  `integrations/esp-idf/libtracer/CMakeLists.txt` (`LIBTRACER_SRCS`) and
  `core/tests/CMakeLists.txt` (`substrate_test_no_atomic`) and
  `tools/cortexm0_footprint.py` (`REQUIRED_MODULES`). CI (esp-idf, the no-atomic
  test, the footprint job) will catch a miss, but it is a wasted round-trip.
- **One focused unit per PR.** Do not fragment a coherent ADR into out-of-order
  stacked pieces to avoid idling; land it whole or hold for a checkpoint.

## 3. Remaining roadmap (each a focused refactor — designs concrete)

Sequencing dependency (from the plan): the sentinel (done) precedes the template
work it referees; rope-aware decode (W2) precedes rope delivery (W3); `key_view`
(done) precedes the ACL cache (W4).

### 3.1 L0 module-set remainder (ADR-0047 §2) — continues #207

#207 landed the mechanism: `segment_t.btag` (inherited from the backend like
`space`), `mem::destroy_dispatch` (`core/src/backend_set.cpp`) — a tag `switch`
to a devirtualized (`final`-class, qualified) direct call per linked backend, with
the virtual `destroy` as the `default:` fallback (so it is byte-identical to the
old `backend->destroy` for every backend). `heap_backend_t` is now public in
`mem_heap.hpp`. `-DLIBTRACER_BACKEND_SET_POOL_ONLY` folds the dispatch to one
direct call (the single-member MCU set; the sentinel builds it).

Two increments remain:

1. **Inline-fold** — turn the current **+20 B** out-of-line `destroy_dispatch`
   seam into a footprint *reduction*. `segment_ptr_t::reset` must **inline** the
   folded `pool_t::destroy`. Blocked by the `mem_pool.hpp` → `segment.hpp` include
   cycle (and `pool_t::destroy` is out-of-line, using `slab_`/`stride_`/
   `free_head_`/`~segment_t()`). The clean break is a **header restructure**:
   forward-declare `segment_t` in `mem_pool.hpp`, and split `segment_ptr_t::reset`
   into a header that can include `mem_pool.hpp` after both types are complete.
   Deterministic but genuinely a restructure — do it as one atomic pass.
2. **`constexpr` traits + `mem::transfer`.** Add `needs_cache_ops` / `is_isr_safe`
   / `owns_bytes` as `static constexpr` on each backend, consumed by
   `mem::transfer(dst, src, io_dir_t)` (host `memcpy` bracketed by `before_io`/
   `after_io` when `needs_cache_ops`; tag-dispatched), which **retires
   `cuda_copy_from_host`/`_to_host`** and gives `io_dir_t`/`before_io`/`after_io`
   their first in-tree callers (review finding #8). **Caveat:** `transfer`'s only
   callers today are the CUDA copy sites (device-only, `LIBTRACER_WITH_CUDA`,
   untestable in CI) — scope so the host path is exercised (e.g. a host-to-host
   test) and the device path is a mechanical rehome. The `owns_bytes`
   durable-store assert rides with the ADR-0049 admission door (Wave 4), not here.

### 3.2 Wave 2 — one wire grammar behind a chunk-cursor (ADR-0048)

Unify the two decoders (`frame.cpp` `decode` — vector tree; `tlv_arena.cpp`
`decode_into` — arena) behind **one grammar core + a chunk-cursor** (span cursor +
rope cursor with a ≤16 B straddled-header scratch stack; payload always zero-copy
subviews/sub-ropes). Sinks unchanged (arena / owning-tree / forwarder-peek);
`compose(tlv-tree-of-views) → rope_t` emit dual, `encode = flatten(compose)`; no
nested-rope type. Also: **delete `wire::error_t`** (the grammar returns
`wire::err_t` directly); **remove `kMaxDepth`** per RFC-0006 (nesting is
receiver-resource-bounded, the arena is the bound); amend `reference/08` §cast to
the validating decode. **Conformance vectors are the byte-exact oracle** — the
safest large refactor to attempt. `graph::status_t` stays gated on RFC-0002.

### 3.3 Wave 3 — receivers, rope seam, transport extraction

- **fn-ptr receivers (ADR-0047 §3).** Replace `std::function` on the delivery path
  with `void (*)(void* ctx, …)` + context — the code-size/no-heap MCU win. The
  surface is large: `transport.hpp` (`receiver_t`, `view_receiver_t`,
  `peer_receiver_t`, `peer_visitor_t`), `fwd_router.hpp` (×10), `transport_vertex`,
  `vertex.hpp` (×4), `graph.hpp` (×6). §3 **couples** this with the owning tier's
  `view_t → rope_t` generalization, which needs W2's rope-aware decode — so land
  W2 first, then the receiver-signature + rope change together, or slice the
  mechanism (fn-ptr, keep `view_t`) first and generalize the payload after.
- **rope seam + framer/POSIX/msquic extraction + WS/CAN zero-copy +
  transport-conformance harness** (findings #4/#5/#6). **`mem_can_reassembly`
  rehome** → `tr::net::can_reassembly_t` beside `transport_can`, `std::map` →
  injected `std::pmr` with bounded groups (evict-oldest + `dropped_groups`
  counter). **`kMaxFrame` → per-connection `:settings`.** **Demote `ws.ts`** —
  already done (#203).

### 3.4 Wave 4 — admission door, ACL, delivery termination

- **Admission door (ADR-0049).** `field_write` becomes THE single SUBSCRIBER
  admission door: the sugars (`graph.cpp`) and `op_resolve.cpp` all route through
  it; uniform SUBSCRIBE ACL gate + transient-local durability latch (a
  **deliberate behavior change**); resolves #59. Uses `key_view` (done).
- **ACL policy (ADR-0050).** Pure per-target policy
  `allows(subject, right, span<const ace_t>, now_ns)` (ALLOW-only MCU / full-DENY
  host, selected by module set) + graph-side **cached effective-ACE merge** with
  generation invalidation on `:acl` writes (merge cached, never the verdict —
  expiry evaluated at check time). Fixes the wrong "control-plane frequency"
  comment (`graph.hpp`).
- **RFC-0007 termination-at-target.** Delivery applies target-local effects and
  never re-dispatches to the target's own `:subscribers[]`; **delete
  `kMaxDispatchDepth` with no replacement** (the `depth` parameter threads out).
  Align `reference/02`/`04` to the shipped model.

## 4. Prompt for a fresh session

Paste this into a new session started in this repo:

```
Continue the libtracer architecture-deepening implementation. Nine PRs
(#199–#207) are merged to main through Wave 1's L0 tag-dispatch mechanism
(ADR-0047 §2). Main is clean, green, zero open PRs.

Read first, in order: CLAUDE.md · docs/research/2026-07-04-implementation-handoff.md
(the roadmap + working conventions — §2 and §3) · docs/research/2026-07-04-architecture-deepening-review.md
(the plan) · the memory files (l0-module-set-design, wave-pr-train,
merge-authorization, compile-time-doctrine, no-synthetic-limits).

Then pick up the next unit from §3 of the handoff doc. Work ONE focused unit per
PR: branch → implement → run the full local gauntlet (build + 27 ctest +
ASan+UBSan + TSan + conformance + tools/cortexm0_footprint.py) → open a signed
(-s, no Co-Authored-By) PR → wait for CI green → merge with
`gh pr merge <N> --merge --admin --delete-branch`. I authorize --admin for
self-merge. Local clang-format is 14 but CI is 18 — match 18 (hand-fix or
pip install clang-format).

Suggested next: EITHER the Wave 2 grammar+cursor unification (ADR-0048 — the
conformance vectors are a byte-exact oracle, so it is the safest large refactor),
OR finish L0 §2 (the inline-fold header restructure, then the constexpr traits +
mem::transfer). Your call on sequencing per §3's dependency notes; do it as one
atomic, fully-validated pass and don't fragment a coherent ADR across stacked PRs.
```
