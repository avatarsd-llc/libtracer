/**
 * @file
 * @brief Per-target build configuration as plain C++ (ADR-0068): every compile-time
 *        knob is an `inline constexpr` constant or a `using` policy binding — never a
 *        preprocessor definition.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This file is ORDINARY, hand-written C++ and is the only place the defaults are spelled.
 * A raw `-Icore/include` consumer (the Cortex-M0 footprint gate, vendored source drops)
 * therefore builds with stock settings and no build-system participation at all.
 *
 * **Overriding.** A target that wants non-default knobs puts a header named
 * `libtracer/config_override.hpp` earlier on the include path; this file picks it up
 * automatically (see @ref tr::graph::default_config_t) and the fragment binds
 * @ref tr::graph::config_t to its own traits type. The fragment is a handful of lines
 * that INHERITS the defaults, so it states only what differs and a knob added here later
 * reaches it untouched. One file per build ⇒ every TU agrees ⇒ no per-TU `-D` mismatch and
 * no ODR hazard — the property the older generated-header arrangement existed to provide,
 * kept without a template, a checked-in second copy, or the configure-time drift gate that
 * guarded the two against each other (ADR-0068 §Erratum 1).
 *
 * Adding a knob: add it HERE as a @ref tr::graph::default_config_t member, never as a
 * macro in a public header and never as a loose constant that a fragment cannot reach.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace tr {

struct mutex_guard_t;  // guard_mutex.hpp — the host guard: address-striped locks (hosted)
struct no_guard_t;     // guard.hpp — guards nothing; single-threaded builds only

}  // namespace tr

namespace tr::graph {

/**
 * @brief Deprecated alias of @ref tr::mutex_guard_t, kept for one release (#1703).
 * Deprecated: The guard vocabulary moved to the layer-neutral `tr` namespace; name
 *             `tr::mutex_guard_t`.
 */
using mutex_guard_t = ::tr::mutex_guard_t;
/**
 * @brief Deprecated alias of @ref tr::no_guard_t, kept for one release (#1703).
 * Deprecated: Name `tr::no_guard_t`.
 */
using no_guard_t = ::tr::no_guard_t;

struct allow_only_policy_t;  // security_acl.hpp — the ALLOW-only MCU profile (ADR-0020 subset)
struct full_acl_policy_t;    // security_acl.hpp — ordered first-match-per-bit with DENY
class hazard_slot_t;         // lkv_slot.hpp — lock-free atomic<node*>; hazard-pointer reclamation
class single_writer_slot_t;  // lkv_slot.hpp — one value_t* swapped inside guard_t; no spin
struct reclaim_strict_t;     // reclaim.hpp — grace point: `unsubscribe()` returns
struct reclaim_local_t;      // reclaim.hpp — grace point: this thread's dispatch stack unwinds
struct reclaim_qsbr_t;  // reclaim.hpp — grace point: EVERY thread has passed a quiescent state

/**
 * @brief The target's build configuration, as ONE named type (ADR-0070).
 *
 * Every compile-time knob is a member here, and the loose names below are DERIVED from it.
 * That ordering is the point: the configuration is a single diffable entity an application can
 * name, pass to a test, and assert on — rather than a scatter of independent declarations that
 * can only be read one at a time.
 *
 * **It is bound once per build, not threaded as a template parameter.** ADR-0070 records why,
 * with measurements: threading it produces byte-identical machine code (verified across eight
 * knob combinations, five optimization levels and two targets), so it buys no latency; its one
 * unique capability — two configurations in one binary — would FORK the process-global stripe
 * and hazard tables, costing exactly the RAM the configuration exists to save; and an
 * app-declared traits type cannot reach the library's out-of-line translation units anyway, so
 * it would layer on this header rather than replace it.
 *
 * **Declaring your own.** Write a `libtracer/config_override.hpp` and put its directory ahead
 * of `core/include` on the include path. This header includes it — after this struct, so the
 * defaults are already visible to inherit from — and uses whatever @ref config_t it binds:
 *
 * ```cpp
 * // libtracer/config_override.hpp
 * #pragma once
 * namespace tr::graph {
 * struct my_node_config_t : default_config_t {
 *     static constexpr std::size_t kCacheLineBytes = 0;  // single-core: no false sharing
 *     using guard_t = my_rtos_critical_section_t;
 * };
 * using config_t = my_node_config_t;
 * }  // namespace tr::graph
 * ```
 *
 * Inheriting from @ref default_config_t means a knob added later does not break your preset —
 * it inherits the new default instead of failing to compile. Stating only the differences is
 * also what keeps the override honest: there is no second copy of the defaults to rot.
 */
struct default_config_t {
    /**
     * @brief The number of lock stripes shared by every vertex in the process (#361 §2).
     *
     * Override fragment: `static constexpr std::size_t kVertexLockStripes = 8;`; ESP-IDF:
     * menuconfig `CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES`, which writes exactly that line. A
     * small single-core node reclaims RAM at 4-8.
     *
     * What N costs, precisely: `N * sizeof(vertex_stripe_t)` bytes of `.bss` reserved at LINK
     * time (plus the same for the condvar table) — the table is not lazy, whatever the platform
     * does. What IS lazy is the platform primitive behind each handle: on FreeRTOS a stripe's
     * mutex costs ~90 B of heap on its first lock, so an untouched stripe costs its struct and
     * no heap. Most of the struct is padding, and @ref kCacheLineBytes decides how much.
     */
    static constexpr std::size_t kVertexLockStripes = 16;

    /**
     * @brief The target's cache-line size for false-sharing padding — or **0 where false
     *        sharing cannot happen**, because the target has no second core to share with.
     *
     * libtracer pads the shared tables whose slots unrelated threads hit concurrently (the
     * vertex lock stripes; the hazard domain's cells and retire lists) up to this boundary, so
     * two threads working on two slots never fight over one line. On a single-core node that
     * padding buys nothing and the bytes are pure loss: measured on rv32 (`-Os`, real
     * `core/src/graph.cpp`, GCC 15.2), 16 stripes cost **1,024 B of `.bss` at 64 and 128 B at
     * 0** — 896 B of a single-core node's static RAM spent against a hazard it does not have.
     *
     * This is an OPTIMIZATION knob, never a correctness one: 0 on a multi-core target costs
     * throughput under concurrent control-plane verbs and changes no observable behaviour.
     * Override fragment: `static constexpr std::size_t kCacheLineBytes = 0;`. The ESP-IDF
     * component derives it from `CONFIG_FREERTOS_UNICORE` — a unicore build has no second
     * core by construction, so the right value is not a question the integrator should be
     * asked.
     *
     * Values below a padded type's natural alignment are raised to it, not applied
     * ([dcl.align]/5 makes a reduction ill-formed, and GCC ignores it *silently*); each padded
     * type static_asserts that the alignment it asked for is the one it got.
     */
    static constexpr std::size_t kCacheLineBytes = 64;

    /**
     * @brief How many threads may hold a hazard announcement at once (ADR-0069 §3).
     *
     * Read this as "threads that concurrently touch a vertex's LKV" — readers announce, writers
     * park displaced nodes, and both claim one index for the life of the thread. A per-target
     * knob rather than a thread ceiling picked out of the air (RFC-0006); override fragment:
     * `static constexpr std::size_t kHazardReaderSlots = 24;`.
     *
     * Sizing: one index per such thread, and **nothing at all** unless @ref lkv_slot_t is bound
     * to `hazard_slot_t` — the default binding (`single_writer_slot_t`) never references the
     * registry, so it is never emitted.
     * Undersizing is not a correctness problem: threads past the bound share one reserved index
     * under a spin lock, so they serialize with each other and with nobody else.
     *
     * What the domain costs when it IS bound, measured on rv32 at N = 64 (`-Os`, real
     * `core/src/graph.cpp`, GCC 15.2) — the padding knob dominates it:
     *
     * | @ref kCacheLineBytes | registry `.bss` | TU `.bss` + `.sbss` |
     * | ---: | ---: | ---: |
     * | 64 | 8,384 B | 11,649 B |
     * | 0 | 1,828 B | 4,197 B |
     *
     * Note the third term the registry figure does not cover: binding the slot also pulls in
     * roughly 2 KB of libstdc++ `__waiter_pool_base` `.bss` (the `atomic::wait` back-end),
     * which is why the TU column is not just the registry plus the stripes.
     */
    static constexpr std::size_t kHazardReaderSlots = 64;

    /**
     * @brief How many threads may hold an EDGE PIN at once (#635) — the per-participant
     *        announcement the fan-out snapshot claims while it copies a vertex's published
     *        edge array out.
     *
     * Read this as "threads that may publish (`graph_t::write`) concurrently". Each claims one
     * index for the life of the thread; the pin itself is held only across the copy-out, never
     * across a dispatch. A per-target knob rather than a thread ceiling picked out of the air
     * (RFC-0006); override fragment: `static constexpr std::size_t kEdgePinSlots = 24;`.
     *
     * **Correctness never depends on this number** — only scaling does. A thread that finds
     * every index taken falls back to copying the CURRENT array under the vertex stripe mutex,
     * which is safe for the reason the mutex existed in the first place: displacing an array
     * requires that same lock, so the current array cannot be retired underneath the fallback
     * reader. Undersizing costs those threads exactly what every thread paid before #635.
     *
     * Unlike @ref kHazardReaderSlots this registry is ALWAYS emitted — the publish path is not a
     * policy binding. Its `.bss` is `N * max(kCacheLineBytes, alignof(void*))` bytes: at N = 32
     * that is 2,048 B on a host (64-byte padding) and 256 B on a single-core MCU profile, which
     * sets @ref kCacheLineBytes to 0 and has no second core to false-share against.
     */
    static constexpr std::size_t kEdgePinSlots = 32;

    /**
     * @brief The RAM-diet RATCHET on `sizeof(vertex_t)`, 64-bit targets (#361 §8).
     *
     * Pinned to the size actually measured, with NO headroom, so the next byte added is a
     * build failure. That is the point: the goal is a lean vertex, and a ceiling held above
     * the measured size cannot express it. A ceiling only answers "did you regress past a
     * fixed point"; both 112 B and 96 B satisfied the old 120 B ceiling, so the 16 B the diet
     * won after #380 §1 were invisible to the build and free to be spent again by anyone.
     * Pinned to the measurement, every byte reclaimed is kept by construction.
     *
     * History, newest first: 88 unchanged by #1621 (the 32-bit write sequence frees 4 B that
     * the member order's tail padding absorbs on a 64-bit host), 88 (RFC-0028 slice 3 — the LKV
     * slot is one `value_t*` word, where the `std::shared_ptr<const rope_t>` it replaced was two;
     * both `lkv_slot_t` bindings agree), 96 (measured across all three CI legs — `acl_full` OFF/ON
     * and both `lkv_slot_t` bindings agree), 112 post-#380 §1, 144 post-packing, 168 post-§3, 160
     * post-§2, 248 post-§1, 536 pre-split.
     *
     * It lives HERE, in the configuration, because it is a per-target budget — and it is
     * enforced in `%vertex.hpp` beside the type it constrains, so **every** build on **every**
     * target evaluates it under **its own** binding. It used to sit in a test, which meant it
     * gated exactly one configuration and never the 32-bit one at all (no CI leg compiled that
     * test cross-target, while the ESP-IDF legs compiled `vertex_t` itself on every PR).
     *
     * Two rules follow from pinning it. RAISING one is a reviewed decision, never a way to
     * make a build pass. LOWERING one is the routine half: a change that shrinks `vertex_t`
     * lowers the number in the same commit, or the gain is handed back to the next author.
     */
    static constexpr std::size_t kMaxVertexBytes64 = 88;

    /**
     * @brief The RAM-diet RATCHET on `sizeof(vertex_t)`, 32-bit (MCU) targets.
     *
     * Pointer-halved, and pinned to the measurement on the same terms as
     * @ref kMaxVertexBytes64 — measured 64 B on rv32 (`-Os -fno-exceptions -fno-rtti`,
     * `rv32imac_zicsr_zifencei`/`ilp32`).
     *
     * Lowered 72 -> 64 by #1621 (RFC-0028 D6): `write_seq_` became a 32-bit atomic. The 64-bit
     * one was 8 B wide AND 8-aligned on rv32, so it cost 4 extra bytes plus 4 of alignment
     * padding, and a libatomic call (`__atomic_fetch_add_8`, which masks interrupts on ESP-IDF)
     * on every publish.
     *
     * This number was 80 with a note claiming rv32 was "exactly 80" and had zero headroom.
     * It was 72 by then: the struct had shrunk and the prose had not, which is exactly the
     * drift a pinned number cannot have — the assert is re-derived from a measurement, while
     * a sentence describing the size is only as true as the day it was written.
     *
     * Earlier: raised 72 -> 80 with the #380 §2 name-key SBO, whose inline buffer adds <= 8
     * struct bytes on 32-bit but deletes a ~32 B heap block per named vertex — and on this
     * target the heap is what is actually scarce.
     */
    static constexpr std::size_t kMaxVertexBytes32 = 64;

    /**
     * @brief The copy-or-share threshold (RFC-0028 §5.3, D3): a written value of AT LEAST this
     *        many bytes is SHARED, one below it is COPIED into the value's own block.
     *
     * The default every vertex answers until its `vertex_policy_t::share_threshold_bytes` gives it
     * its own. At ingress, "shared" means the stored value links the inbound receive segment
     * (refcount, zero copy) and "copied" means the bytes land inline in the one `value_t` block
     * the publish costs (`value_t::make_inline`: one allocation, one `memcpy`). A payload whose
     * TLV carries a CRC/TS trailer is always copied — a shared frame's opt byte cannot be
     * patched — and so is one whose reader cannot share (a borrowed, span-delivered frame).
     *
     * `0` shares always; `SIZE_MAX` copies always. The two ends are the old `kPinNever` and
     * "pin unconditionally", now edge cases of one comparison on the absolute payload size —
     * the variable that was actually measured (RFC-0028 R6); the retired ratio was the
     * measurement rig's rotation knob (#774).
     *
     * @section share_borrow Sharing BORROWS the receive segment, and this is the knob that prices
     * it
     *
     * A shared value holds its whole inbound RX **segment** for as long as it is the vertex's
     * last-known value (or sits in a STREAM ring). On a pooled RX backend that is a **pool
     * slot** — receive capacity the transport cannot use until the value is displaced. The
     * library makes the deferred release *safe*; only the application knows its pool geometry
     * and retention pattern, so the budget is the consumer's number (RFC-0022 Amendment 2).
     * Size against `live shared values x segment_bytes`; `bench/README.md` §"RFC-0022 §6 —
     * receive-pool occupancy" measured a 29-slot ESP32-C6 pool collapsing once the live shared
     * set crossed the slot count.
     *
     * Target-class defaults (§11 ruling 2):
     * - **host (WIDE / MID)** — 4,096 B, RFC-0022 Amendment 2's knee: below it the copy is
     *   cheaper than the borrow, above it the zero-copy share wins.
     * - **NARROW** — a per-build trait: a fixed, small RX pool states its own value in its
     *   `config_override.hpp` (the ESP-IDF component binds `SIZE_MAX`, copy always, which is
     *   exactly the posture it shipped before this knob).
     */
    static constexpr std::size_t kShareThresholdBytes = 4096;

    /**
     * @brief The largest block the process-default heap backend draws for ONE segment (#1768):
     *        a segment whose padded header plus payload fits is one block, a larger one is
     *        drawn as two — the payload, then the bare header.
     *
     * RFC-0028 slice 10 put the `segment_t` header and the payload in one heap block, which
     * made a 64 B value about 30% cheaper. It also moved every payload within one header of a
     * host allocator's small-block ceiling over it: a 1024 B value asked for 1072 B, and on
     * glibc that is past the per-thread cache, so `lkv-store-heap 1024B` doubled (27 → 54 ns).
     * Two draws that each fit the fast path are cheaper than one that misses it, so above this
     * threshold `tr::mem::heap_backend_t` splits; at or below it the one-block layout stays.
     * The split changes nothing a caller can see — the segment, its payload alignment and its
     * reclaim are the same; only the number of `operator new` calls differs.
     *
     * **Default 1,032 B — glibc's tcache ceiling on a 64-bit host.** glibc's per-thread cache
     * has 64 bins 16 B apart, and the largest request it serves is 1,032 B (`MAX_TCACHE_SIZE`,
     * the `glibc.malloc.tcache_max` tunable's default). That is the cliff measured on the
     * reference host, so it is the number to split at. A host allocator without a cliff
     * there loses only one extra draw on a value of about 1 KiB or more, where the payload
     * copy already costs more than the draw. The figure is 64-bit glibc's; a 32-bit host's
     * ceiling is lower, and such a host states its own value.
     *
     * Applies ONLY to `heap_backend_t`. An injected source (`source_backend_t`), the pool and
     * the borrowed backends keep their one-block layout: a size-classed source wants exactly
     * one draw per segment, and that is what it is sized against.
     *
     * Target-class defaults:
     * - **host (WIDE / MID)** — 1,032 B, the glibc ceiling above.
     * - **NARROW** — a per-build trait. An allocator with no small-block fast path (ESP-IDF's
     *   `multi_heap`) gains nothing from a second draw, so the ESP-IDF component binds
     *   `SIZE_MAX`, one block always — the layout it shipped in v0.17.0.
     *
     * Override fragment: `static constexpr std::size_t kHeapSmallBlockBytes = 1032;`.
     */
    static constexpr std::size_t kHeapSmallBlockBytes = 1032;

    /**
     * @brief The target's selected ACL policy (ADR-0047 §1 build-time module set).
     *
     * Default: the ALLOW-only MCU profile. The CMake option `LIBTRACER_ACL_FULL=ON` rebinds
     * this to the full `security_acl` host policy (ordered first-match-per-bit with DENY) — a
     * target-configuration change, never an edit to `graph.cpp`.
     */
    using acl_policy_t = allow_only_policy_t;

    /**
     * @brief The target's ONE critical-section type (RFC 0028 §5.5): the guard
     *        `single_writer_slot_t` opens around its pointer swap and its handle copy, AND the
     *        default `Sync` policy of `tr::mem::synchronized_pool_t`.
     *
     * One trait, not two: before slice 10 the pool had its own `pool_sync_policy` vocabulary
     * (`spin_sync_t` / `portmux_sync_t`), bound separately from this one. It must model
     * `tr::guard` (`%guard.hpp`): `lock()` / `unlock()`, a static `for_address(const void*)`
     * the slot takes, and the `is_isr_safe` / `is_nonblocking` / `may_spin` / `name` traits.
     * It also serializes the write-sequence bump on a core with no atomic read-modify-write
     * (`tr::rmw_counter_t`).
     *
     * Spelled `reader_guard_t` until #1703. A fragment that still defines that name is refused
     * at compile time (see the tripwire after @ref config_t), because a fragment whose binding
     * the library no longer reads would fall back to `mutex_guard_t` without a word.
     *
     * `mutex_guard_t` by default: a table of address-striped one-word locks — one RMW to take,
     * a release store to give back — whose contender re-reads briefly and then sleeps instead
     * of spinning on a descheduled holder; unrelated vertices rarely share one. A single-core
     * RTOS build binds an interrupt-masked critical section (the ESP-IDF component:
     * `tr::esp::critical_guard_t`). The guard must never spin-wait where @ref kSpinWaitSafe is
     * `false` — that is the whole of #1618 — and a guard that declares `may_spin` is refused
     * there by both the slot and the pool.
     */
    using guard_t = ::tr::mutex_guard_t;

    /**
     * @brief The target's selected LKV slot policy (ADR-0069 §1).
     *
     * How a vertex publishes and reads its last-known value. Default: `single_writer_slot_t`,
     * one `value_t*` swapped and retained inside @ref guard_t. It has no registry, no
     * deferred reclamation and a publish that cannot fail, and its one wait is the guard (#1618).
     *
     * `hazard_slot_t` is the lock-free alternative for a host whose reads of one shared vertex
     * contend across many cores. Override fragment: `using lkv_slot_t = hazard_slot_t;`. The
     * named type must satisfy the contract in `%lkv_slot.hpp` — in particular `load()` returns an
     * OWNING handle, and the policy declares `may_spin`, which is refused where
     * @ref kSpinWaitSafe is `false`.
     */
    using lkv_slot_t = single_writer_slot_t;

    /**
     * @brief Force the guarded binding of every `rmw_counter_t` a vertex owns, even where the
     *        counter's width is natively lock-free (#1715).
     *
     * A test knob: it lets a host build exercise the path a target without a native atomic RMW
     * takes, where the write-sequence bump runs under @ref guard_t and is fused into the LKV
     * publish section. Production code leaves it `false`; the native binding is then selected
     * exactly as before and its generated code is unchanged.
     */
    static constexpr bool kForceGuardedRmw = false;

    /**
     * @brief The target's selected RECLAMATION policy (ADR-0080) — WHEN the library may free
     *        the memory behind a retired subscription's `{fn, ctx}` pair.
     *
     * Default: `reclaim_local_t`, whose grace point is "this thread's dispatch stack unwinds
     * to depth 0". It is the default because it makes the MCU and the host build behave
     * IDENTICALLY: `reclaim_strict_t` forbids unsubscribing from inside a delivery, which
     * would make the same application code legal on a host and illegal on the constrained
     * target — a portability bug that surfaces only where it is hardest to debug.
     *
     * What the default costs, and what rebinding buys, is one non-atomic increment,
     * decrement and branch per `fan_out` on a per-thread counter — once per publish,
     * regardless of subscriber count, and nothing at all on a publish nobody subscribed to
     * (the counter sits after `fan_out`'s no-subscriber gate). Override fragment:
     * `using reclaim_policy_t = reclaim_strict_t;` — worth taking only where the deployment
     * can show that re-entrant unsubscribe does not occur, because `reclaim_strict_t` cannot
     * see it outside a debug build.
     *
     * `reclaim_qsbr` — ADR-0080's third policy, whose grace point spans EVERY thread — is the
     * one to bind when this node dispatches from several threads at once and unsubscribes from
     * another, the case neither policy above covers. Override fragment:
     * `using reclaim_policy_t = reclaim_qsbr_t;`. It prices one relaxed load and one `seq_cst`
     * store per OUTERMOST fan-out (not per edge), and it is the only policy whose release hook
     * may run on a thread other than the `unsubscribe()` caller — see `%reclaim.hpp`.
     */
    using reclaim_policy_t = reclaim_local_t;

    /**
     * @brief How many retired `{ctx, release}` pairs ONE THREAD may hold parked at once, when
     *        @ref reclaim_policy_t defers (ADR-0080).
     *
     * Read this as "subscriptions unsubscribed from INSIDE a single delivery stack". The
     * ordinary unsubscribe — from outside any callback — parks nothing at all, so on most
     * nodes this storage is touched zero times; it exists for the re-entrant case
     * `reclaim_local_t` supports and `reclaim_strict_t` forbids.
     *
     * Sizing: one @ref retired_callback_t per slot, in per-thread storage that is plain bytes
     * with no destructor and no allocation — 256 B on a 64-bit host at the default, 128 B on a
     * 32-bit MCU, and only on a thread that actually dispatches. Nothing at all under
     * `reclaim_strict_t`, which never defers.
     *
     * Overflow is a REFUSAL, not a failure: a pair that finds every slot taken is DROPPED and
     * its hook is never run — a leak, deliberately, because the alternative is running a
     * release hook while the fan-out that is still walking the snapshot names that context.
     * `graph_t::deferred_release_drops()` counts every one, so an undersized bound is
     * observable rather than silent. Override fragment:
     * `static constexpr std::size_t kDeferredReleaseSlots = 64;`
     *
     * **Under `reclaim_qsbr_t` read it differently.** There the pairs are held across a
     * cross-thread GRACE PERIOD rather than a dispatch stack, in ONE shared table rather than
     * per-thread storage, so the quantity it bounds is "retirements in flight process-wide
     * while some participant has yet to quiesce" — a larger and less predictable number. Raise
     * it (64 is a sane starting point) on any node that unsubscribes in bulk while other
     * threads dispatch. The overflow rule is identical, and so is its observability: a QSBR
     * build retries the scan once after publishing the pair, so a drop means a participant
     * thread genuinely never reached a quiescent state — an embedder defect
     * @ref tr::graph::graph_t::deferred_release_drops now names.
     */
    static constexpr std::size_t kDeferredReleaseSlots = 16;

    /**
     * @brief How many threads may participate in the `reclaim_qsbr_t` grace period at once
     *        (#1376) — the per-thread quiescent-state announcement a retiring thread scans.
     *
     * Read this as "threads that may dispatch (`fan_out`, or an ADR-0049 durability latch)
     * concurrently". Derived from @ref kEdgePinSlots rather than picked out of the air, the way
     * `lkv_slot.hpp`'s `kRetireBatch` is derived from @ref kHazardReaderSlots — the two sets are
     * the same threads, since a thread that publishes is a thread that dispatches.
     *
     * **Unlike @ref kEdgePinSlots, correctness is not indifferent to this number** — but it
     * fails SAFE, not silently. A thread that finds every index taken cannot announce itself,
     * and a thread a scan cannot see is one no grace period may conclude past; so it counts
     * itself into an overflow tally instead, and any non-zero reading blocks all reclamation
     * until it clears. The failure mode is therefore deferred frees (visible as
     * @ref tr::graph::graph_t::deferred_release_drops rising), never a use-after-free.
     *
     * Its `.bss` is `N * max(kCacheLineBytes, alignof(std::uint64_t))` bytes — at N = 32 that
     * is 2,048 B on a host — plus the shared retired table. **It is emitted only in a build
     * that actually binds `reclaim_qsbr_t`**: `%graph.cpp` reaches the domain exclusively from
     * `if constexpr` branches a non-QSBR build discards, and GCC emits nothing at all for those
     * — 0 symbols and 0 B of `.bss`, verified. Override fragment:
     * `static constexpr std::size_t kQsbrParticipants = 64;`
     */
    static constexpr std::size_t kQsbrParticipants = kEdgePinSlots;

    /**
     * @brief How many `DEVICE`-space memory backends may register a transfer hook at once
     *        (#1381) — the bound on `tr::mem::register_device_backend`'s table.
     *
     * An L0 (`tr::mem`) fact, here for the same reason @ref kSpinWaitSafe is: ADR-0070's rule
     * is that the configuration is ONE named type. @ref tr::mem::kDeviceBackendSlots is its
     * spelling for the memory layer.
     *
     * Read it as "vendor accelerator backends this process binds" — a GPU tier module, an NPU
     * one, a dmabuf one. Two is the honest default: a host that talks to one accelerator family
     * needs one slot, and nothing in-tree registers at all.
     *
     * Its `.bss` is `N * (sizeof(void*) + sizeof(void(*)()))` — 32 B on a 64-bit host at the
     * default — and it is emitted **only** in a build that links `device_backend.cpp`. A
     * single-backend (`LIBTRACER_BACKEND_SET_POOL_ONLY`) target has no device arm and never
     * links that TU, so the cost there is 0 B rather than "small". Override fragment:
     * `static constexpr std::size_t kDeviceBackendSlots = 4;`
     *
     * Overflow is a REFUSAL, not a failure: `register_device_backend` returns `false` and
     * registers nothing, so a backend that could not claim a slot moves no bytes at all
     * (`tr::mem::transfer` answers `false` for its segments) rather than silently sharing
     * another vendor's hook.
     */
    static constexpr std::size_t kDeviceBackendSlots = 2;

    /**
     * @brief Whether a task on this target may SPIN-WAIT for a lock another task holds (#1158).
     *
     * An L0 (`tr::mem`) fact, but a member HERE because ADR-0070's rule is that the
     * configuration is ONE named type: a knob that lives outside it cannot be set by an
     * override fragment, which is exactly the defect that kept this one in the build system.
     * @ref tr::mem::kSpinWaitSafe is its spelling for the memory layer, derived like every
     * other loose name below.
     *
     * True on a multi-core host: the holder runs on a different core, so a spinner makes
     * progress possible and the O(1) section costs less than a mutex round-trip. FALSE on a
     * priority-preemptive scheduler, where a spinner that outranks the holder never yields the
     * CPU the holder needs to release the lock — the wait becomes unbounded priority inversion
     * and the board hangs in the watchdog rather than merely running slowly. That is true of a
     * single-core chip and equally of an SMP chip whose spinner and holder share a core.
     *
     * Asserted against the pool's sync policy (`%mem_pool.hpp`) and against every LKV slot
     * policy's `may_spin` (`%vertex.hpp`, #1618).
     */
    static constexpr bool kSpinWaitSafe = true;

    /**
     * @brief Whether this target carries the ADR-0044 BUS facet at all — peer-named links,
     *        per-peer addressing, in-band peer enumeration (#375 deliverable 3).
     *
     * A `tr::net` fact, and a member HERE for the reason @ref kSpinWaitSafe and
     * @ref kDeviceBackendSlots are: ADR-0070's rule is that the configuration is ONE named
     * type, so a knob that lives outside it cannot be reached by an override fragment.
     * @ref tr::net::kBusLinks is its spelling for the transport plane.
     *
     * **What it closes.** A bus link reaches MANY peers and names each of them, so the
     * routing plane carries a second addressing tier for it: the registry stamps a mount's
     * bus SHAPE and resolves a residual segment as a peer (`child_registry_t::resolve_peer`,
     * `by_name`'s peer fallback), `fwd_router_t::add_child` wires the peer-named receiver and
     * both peer-lifecycle notifiers, and a connection vertex synthesizes its `:children[]`
     * from the link's live peer table. Bound `false`, every one of those consumers folds to
     * the point-to-point answer at COMPILE time through `%tr::net::bus_of` (`%transport.hpp`), and
     * the peer-named machinery behind them is never reached — ADR-0047 §1 link-time module
     * selection, expressed as a configuration member rather than as a TU list, because
     * whether a tcp/ws listener is peer-named is a WIRING-time choice inside a TU that a
     * bus-less target still compiles for its point-to-point half.
     *
     * **What it costs to carry and what the default saves.** Measured on rv32
     * (`-Os -fno-exceptions -fno-rtti`, `rv32imac_zicsr_zifencei`/`ilp32`, GCC 15.2, per-TU
     * `.text`), closing it removes **1,400 B** of flash from `%fwd_router.cpp` and **678 B**
     * from `%transport_vertex.cpp` — 2,078 B — and **0 B** of `.bss`, because the tier is code
     * and per-instance state, not a static table. A `LIBTRACER_NET_PLANE=OFF` build gains
     * nothing: it never compiled those TUs in the first place.
     *
     * **Default `false` — the lean choice (#1670, v0.17.0).** A node whose links are
     * point-to-point — one dial upstream, or a listener that serves its peers as one broadcast
     * link (ADR-0001's originating firmware shape) — pays nothing for a tier it never uses.
     *
     * **Who opts in.** A node that runs a peer-named listener (`peer_named=true` tcp/ws), the
     * ESP-IDF WS server `httpd_ws_link_t`, or ANY CAN link — the last two are buses by
     * construction. Override fragment: `static constexpr bool kBusLinks = true;` — the core
     * test build, the `bench/` build and the ESP-IDF `CONFIG_LIBTRACER_BUS_LINKS` (which
     * `CONFIG_LIBTRACER_WS_SERVER` selects) do exactly that.
     *
     * **It is a REFUSAL, never a silent downgrade.** A build that binds it `false` and then
     * asks for a bus is rejected, loudly and at the earliest door that can speak: compiling
     * `LIBTRACER_TRANSPORT_CAN` (a bus by construction) is a `static_assert`, and a
     * `peer_named=true` tcp/ws listener is refused by its SPEC factory and reports
     * `transport_t::ok() == false` when constructed directly. Quietly serving such a
     * configuration as FLAT would be worse than either: the listener's own per-frame tier
     * select would keep delivering peer-named into a sink the router never installed.
     */
    static constexpr bool kBusLinks = false;

    /**
     * @brief Whether this target carries the RFC-0014 §4 S5 LINK-LIVENESS ENGINE at all —
     *        `self_heal_link_t`, its worker thread and its backoff/redial state machine
     *        (#1470).
     *
     * A `tr::net` fact, and a member HERE for the reason @ref kBusLinks is: ADR-0070's rule
     * is that the configuration is ONE named type, so a knob that lives outside it cannot be
     * reached by an override fragment. @ref tr::net::kSelfHealLinks is its spelling for the
     * transport plane.
     *
     * **What it closes.** The engine is minted on exactly one path — a `config`-`kind` DIAL
     * whose kind was registered with `transport_kind_traits_t::self_heal_dial`
     * (`transport_vertex.cpp`, `create_connection_locked`). Bound `false`, that mint is
     * discarded at COMPILE time, nothing references `self_heal_link_t`, and the TU is
     * dropped from the archive by the build lists that also gate it
     * (`LIBTRACER_SELF_HEAL_LINKS` in `core/CMakeLists.txt`,
     * `CONFIG_LIBTRACER_SELF_HEAL_LINKS` in the ESP-IDF component).
     *
     * **What it costs to carry and what the default saves.** Measured by the reporter on an
     * ESP32-C6 image (riscv32, `-Os -fno-exceptions -fno-rtti`, same sdkconfig) across the
     * release that made the TU unconditional: `nm` on the linked ELF finds **4,336 B** of
     * reachable `self_heal_link_t` symbols (`worker_main`, `attempt_locked`, `reap_locked`,
     * …) out of a +6,224 B image bump, and **0 B** of `.dram0.bss` — the engine is code and
     * per-instance state, not a static table.
     *
     * **Default `false` — the lean choice (#1670, v0.17.0; re-rules #1548's default).** The
     * built-in `udp`/`tcp`/`ws` DIAL kinds then dial EAGERLY, exactly as they did before
     * #1548 made them engine-managed: the connection comes up at creation, with no redial,
     * no backoff and no liveness publishing beyond `UP`.
     *
     * **Who opts in.** A node that wants its config-created DIAL connections minted
     * `DORMANT` and self-healing, or that registers its own `self_heal_dial` kind. Override
     * fragment: `static constexpr bool kSelfHealLinks = true;` — AND compile the TU
     * (`-DLIBTRACER_SELF_HEAL_LINKS=ON`; the ESP-IDF `CONFIG_LIBTRACER_SELF_HEAL_LINKS` does
     * both from one symbol). The core test build and the `bench/` build opt in.
     *
     * **It is a REFUSAL, never a silent downgrade.** A build that binds it `false` and then
     * registers a `self_heal_dial` kind is rejected at `register_transport_type`: the kind
     * is NOT catalogued, so a `SPEC` naming it answers `SCHEMA_NOT_FOUND` — the same answer
     * any unregistered kind gets — and a debug build asserts at the registration itself.
     * Quietly registering it with the trait CLEARED would be worse than either: the
     * connection would come up eagerly, with no redial and no liveness publishing, and
     * nothing would say so.
     *
     * **The built-ins are not subject to that refusal, and are not downgraded by it**
     * (maintainer ruling 2026-08-25, #1548): they read this knob at their OWN registration
     * site (`%kBuiltinPointToPointTraits` in `%builtin_transports.hpp`) and declare
     * `self_heal_dial = false` on a closed-out build, keeping the eager dial they always
     * had. The refusal above targets a kind that CLAIMS an engine the image does not carry;
     * a build-conditioned declaration claims nothing it cannot have. Making the refusal fire
     * for the built-ins instead would drop `udp`/`tcp`/`ws` out of the catalog and turn the
     * lean default into a broken build.
     */
    static constexpr bool kSelfHealLinks = false;

    /**
     * @brief Stack bytes for the link-liveness engine's worker thread; `0` = the platform
     *        default (#1470).
     *
     * The worker is the sole dialer and the sole liveness publisher, so its stack has to
     * carry a `connect()` and the publish fan-out beneath it. On a POSIX host the platform
     * default is megabytes of lazily-committed address space and nobody has to think about
     * it. On an RTOS target it is a REAL allocation from the heap, per link: ESP-IDF's
     * `std::thread` is a pthread, and a pthread takes
     * `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` from a board that may have ~76 KB free —
     * 12,288 B per link on the sdkconfig #1470's reporter measured. There was no way to
     * size it, which made the engine unusable on such a target INDEPENDENTLY of whether the
     * flash was affordable — the second half of #1470.
     *
     * Applied through `pthread_attr_setstacksize` at the spawn, which is the same mechanism
     * `posix_endpoint_t::start` and `socketcan_link_t::start` already use for their receive
     * threads, and which IDF's pthread honours. So it is **not** an ESP-only knob with an
     * inert body elsewhere: glibc honours it too. A value below the platform floor makes
     * `pthread_attr_setstacksize` return `EINVAL` and leaves the default in place — the
     * spawn still happens, with the default stack, exactly as the two precedents behave.
     *
     * Left at `0` — every host build, and the default everywhere — nothing is set and the
     * spawn is byte-for-byte the one that shipped. Override fragment:
     * `static constexpr std::size_t kSelfHealWorkerStackBytes = 4096;`
     */
    static constexpr std::size_t kSelfHealWorkerStackBytes = 0;

    /**
     * @brief Whether @ref tr::graph::graph_t carries its two INSTRUMENTATION counters —
     *        `ancestor_walks()` and `target_canonical_resolves()` (#1664).
     *
     * Both are relaxed 64-bit `fetch_add`s on a node-wide counter that nothing in the library
     * reads: no `:stats` noun, no wire surface, no decision. Only tests and benches consume
     * them, as ablations proving a write took the leg its name claims — the bubbling walk
     * (RFC-0005) and the target-edge canonical fallback (#830). Paying for them on a shipped
     * node is pure loss, and the loss is not uniform: on rv32 a 64-bit atomic RMW is a
     * libatomic call, taken once per write that has an ancestor subscriber and once per
     * unbound target-edge delivery.
     *
     * **Default `false` — the lean choice.** Closed out, the two members occupy no bytes of
     * `graph_t` (`[[no_unique_address]]` over an empty type), the two increment sites compile
     * to nothing, and the accessors answer `0`. It is a compile-time member rather than the
     * `LIBTRACER_PIN_INSTRUMENT` macro `%pin_instrument.hpp` uses, for ADR-0068's reason: a
     * macro can differ per TU, and these counters change `graph_t`'s layout.
     *
     * **Who sets it.** The core test build and the `bench/` build's
     * `LIBTRACER_INSTRUMENT_COUNTERS` option opt in through the checked-in preset fragment
     * `core/tests/instrumented/libtracer/config_override.hpp`. A test that asserts on either
     * counter gates the assertion on this knob, so a CI leg binding its own fragment (which
     * inherits `false`) still runs the rest of the test. Override fragment:
     * `static constexpr bool kInstrumentCounters = true;`
     */
    static constexpr bool kInstrumentCounters = false;

    /**
     * @brief Whether a connection SPEC may carry the `insecure` key of the `quic` and
     *        `webtransport` kinds — the dial-side switch that skips server-certificate
     *        verification.
     *
     * `insecure` is a DEV-ONLY convenience for reaching a self-signed peer. A SPEC is a
     * wire write, and on a build without an ACL policy any connected peer may write one, so
     * honouring the key unconditionally would let a peer make this node dial with peer
     * authentication switched off. Whether that key is honoured at all is therefore a
     * decision the BUILD states, not the SPEC.
     *
     * **Default `false` — the lean and safe choice.** Closed out, a SPEC whose config carries
     * `insecure` = nonzero is REFUSED at creation: the factory answers
     * `graph::status_t::PERMISSION_DENIED` and counts the refusal
     * (`%tr::net::quic_insecure_refusals()` / `%tr::net::webtransport_insecure_refusals()`, in
     * the respective module header). It is never silently ignored — a dial that asked for no
     * verification and quietly got verification would fail later for a reason nobody wrote
     * down — and never honoured. `insecure` = `0` is the explicit "verify" spelling and is
     * accepted either way. An app TLS profile whose `ca_file` certifies the peer (selected
     * by the SPEC's `tls` key, see `%tr::net::tls_profile_t`) is the way to reach a
     * privately-issued or self-signed peer on a closed-out build.
     *
     * Only the SPEC path is gated. An application that constructs a transport itself with
     * `quic_dial_tls_t{.insecure_no_verify = true}` has made that choice in its own C++; no
     * peer can reach that field.
     *
     * **Who sets it.** A development or test build that dials self-signed peers through a
     * SPEC. The `quic` CI workflow runs the QUIC and WebTransport tests a second time under
     * the checked-in fragment `core/tests/insecure-tls/libtracer/config_override.hpp`.
     * Override fragment: `static constexpr bool kAllowInsecureTls = true;`
     */
    static constexpr bool kAllowInsecureTls = false;
};

}  // namespace tr::graph

// ---------------------------------------------------------------------------------------------
// The override seam. A target with non-default knobs puts `libtracer/config_override.hpp` ahead
// of this directory on the include path; it is included HERE, after default_config_t, so the
// fragment can inherit the defaults and must state only what differs. Absent — the ordinary
// case, and every raw `-I` consumer — the defaults bind unchanged.
//
// __has_include is a DISCOVERY question ("did the integrator supply a file?"), not the
// configuration-by-macro that ADR-0068 retired: no knob is spelled as a preprocessor symbol,
// nothing is per-TU (the fragment is on the include path for the whole build, so every TU
// resolves the same file), and the value that reaches the code is ordinary typed C++.
#if defined(__has_include)
#if __has_include(<libtracer/config_override.hpp>)
#include <libtracer/config_override.hpp>
/** @brief Set by THIS header when a fragment was found — the fragment binds `config_t` itself
 *         and is never asked to define a marker of its own. */
#define LIBTRACER_HAS_CONFIG_OVERRIDE 1
#endif
#endif

namespace tr::graph {

/**
 * @brief THE configuration this build uses — the one binding, and the one thing to override.
 *
 * An override fragment binds this alias to its own traits type; with no fragment present it
 * names @ref default_config_t. Everything below is derived from it, so nothing else changes.
 */
#if !defined(LIBTRACER_HAS_CONFIG_OVERRIDE)
using config_t = default_config_t;
#endif

/**
 * @brief Whether the configuration @p C still defines the pre-#1703 member `reader_guard_t`.
 *
 * The tripwire's predicate. `default_config_t` no longer defines it, so only a stale override
 * fragment can.
 */
template <class C>
concept defines_reader_guard_member = requires { typename C::reader_guard_t; };

// The rename tripwire (#1703). A fragment written before the rename binds `reader_guard_t`,
// which nothing reads any more, so the build would silently keep the host `mutex_guard_t` —
// and on a dual-core chip with a lock-free bool no other assertion catches that. Refuse it.
static_assert(!defines_reader_guard_member<config_t>,
              "config_t defines reader_guard_t, which libtracer no longer reads: the member was "
              "renamed to guard_t (#1703). In your libtracer/config_override.hpp, rename "
              "`using reader_guard_t = ...;` to `using guard_t = ...;`.");

/**
 * @brief Whether the configuration @p C still defines `kWeaklyOrdered`, removed by #1717.
 *
 * The removal tripwire's predicate. `default_config_t` no longer defines it, so only a stale
 * override fragment can.
 */
template <class C>
concept defines_weakly_ordered_member = requires { C::kWeaklyOrdered; };

// The removal tripwire (#1717). `kWeaklyOrdered = false` let a build that declared itself TSO
// waive the delivery-skip order assertion in vertex.hpp; nothing in-tree set it, and that
// assertion is now unconditional. A fragment that still sets it would compile on believing it
// holds a waiver that no longer exists. Refuse it, so the line is deleted.
static_assert(!defines_weakly_ordered_member<config_t>,
              "config_t defines kWeaklyOrdered, which libtracer no longer reads: kWeaklyOrdered "
              "was removed (#1717). The delivery-skip order is asserted seq_cst on every target, "
              "so there is nothing left to waive. Delete `static constexpr bool kWeaklyOrdered = "
              "...;` from your libtracer/config_override.hpp.");

/**
 * @brief Whether the configuration @p C still defines `kSingleWriter`, removed by #1718.
 *
 * The removal tripwire's predicate. `default_config_t` no longer defines it, so only a stale
 * override fragment can.
 */
template <class C>
concept defines_single_writer_member = requires { C::kSingleWriter; };

// The removal tripwire (#1718). `kSingleWriter` was an unchecked promise of one publisher per
// vertex that no code read; the fused guarded publish (#1715) left it nothing to unlock. A
// fragment that still sets it would compile on believing it states a contract the library
// honours. Refuse it, so the line is deleted rather than left to mislead.
static_assert(!defines_single_writer_member<config_t>,
              "config_t defines kSingleWriter, which libtracer no longer reads: kSingleWriter was "
              "removed (#1718). Every LKV slot's guard serializes writers as well as readers, so "
              "the trait unlocked nothing. Delete `static constexpr bool kSingleWriter = ...;` "
              "from your libtracer/config_override.hpp.");

// ---------------------------------------------------------------------------------------------
// Derived spellings. These are what the rest of the library and its consumers actually name;
// they exist so that introducing @ref config_t moved no call site. Each is exactly its traits
// member — do not let one drift into an independent value.

/** @brief @ref default_config_t::kVertexLockStripes for this build. */
inline constexpr std::size_t kVertexLockStripes = config_t::kVertexLockStripes;
/** @brief @ref default_config_t::kCacheLineBytes for this build. */
inline constexpr std::size_t kCacheLineBytes = config_t::kCacheLineBytes;
/** @brief @ref default_config_t::kHazardReaderSlots for this build. */
inline constexpr std::size_t kHazardReaderSlots = config_t::kHazardReaderSlots;
/** @brief @ref default_config_t::kEdgePinSlots for this build. */
inline constexpr std::size_t kEdgePinSlots = config_t::kEdgePinSlots;
/** @brief @ref default_config_t::kShareThresholdBytes for this build. */
inline constexpr std::size_t kShareThresholdBytes = config_t::kShareThresholdBytes;
/** @brief @ref default_config_t::kHeapSmallBlockBytes for this build. */
inline constexpr std::size_t kHeapSmallBlockBytes = config_t::kHeapSmallBlockBytes;
/** @brief @ref default_config_t::kDeferredReleaseSlots for this build. */
inline constexpr std::size_t kDeferredReleaseSlots = config_t::kDeferredReleaseSlots;
/** @brief @ref default_config_t::kQsbrParticipants for this build. */
inline constexpr std::size_t kQsbrParticipants = config_t::kQsbrParticipants;
/** @brief @ref default_config_t::acl_policy_t for this build. */
using acl_policy_t = config_t::acl_policy_t;
/** @brief @ref default_config_t::kInstrumentCounters for this build. */
inline constexpr bool kInstrumentCounters = config_t::kInstrumentCounters;
/** @brief @ref default_config_t::kForceGuardedRmw for this build. */
inline constexpr bool kForceGuardedRmw = config_t::kForceGuardedRmw;
/** @brief @ref default_config_t::guard_t for this build. */
using guard_t = config_t::guard_t;
/**
 * @brief Deprecated alias of @ref guard_t, kept for one release (#1703).
 * Deprecated: Name `tr::graph::guard_t`. (The config MEMBER of that name is not aliased: a
 *             fragment that defines it is refused, above.)
 */
using reader_guard_t = guard_t;
/** @brief @ref default_config_t::lkv_slot_t for this build. */
using lkv_slot_t = config_t::lkv_slot_t;
/** @brief @ref default_config_t::reclaim_policy_t for this build. */
using reclaim_policy_t = config_t::reclaim_policy_t;

}  // namespace tr::graph

// ---------------------------------------------------------------------------------------------
// L0 (tr::mem) build configuration. Delivered by the same header for the same reason (ADR-0068:
// one file per build, so every TU agrees); a separate namespace because the fact it states
// belongs to the memory layer, not the graph.

namespace tr::mem {

/**
 * @brief Whether a task on this target may SPIN-WAIT for a lock another task holds.
 *
 * The memory layer's spelling of @ref tr::graph::default_config_t::kSpinWaitSafe, which carries
 * the full rationale. Derived from @ref tr::graph::config_t exactly as the `tr::graph` loose
 * names are, so an override fragment sets it in the one place every knob is set.
 *
 * A target fact, so the BUILD states it and nothing asks the integrator (the same reasoning
 * that derives @ref tr::graph::kCacheLineBytes rather than exposing it). Its one consumer
 * is the guard in `synchronized_pool_t`, which refuses to instantiate the spinlock policy
 * where spin-waiting is unsafe.
 */
inline constexpr bool kSpinWaitSafe = tr::graph::config_t::kSpinWaitSafe;

/**
 * @brief How many `DEVICE`-space backends may register a transfer hook at once.
 *
 * The memory layer's spelling of @ref tr::graph::default_config_t::kDeviceBackendSlots, which
 * carries the full rationale. Derived from @ref tr::graph::config_t exactly as @ref
 * kSpinWaitSafe is, so an override fragment sets it in the one place every knob is set. Its one
 * consumer is the bounded table behind @ref register_device_backend (`device_backend.cpp`).
 */
inline constexpr std::size_t kDeviceBackendSlots = tr::graph::config_t::kDeviceBackendSlots;

}  // namespace tr::mem

// ---------------------------------------------------------------------------------------------
// Transport-plane (tr::net) build configuration. Delivered by the same header for the same
// reason the L0 block above is (ADR-0068: one file per build, so every TU agrees); a separate
// namespace because the fact it states belongs to the transport plane, not the graph.

namespace tr::net {

/**
 * @brief Whether this target carries the ADR-0044 BUS facet at all (peer-named links).
 *
 * The transport plane's spelling of @ref tr::graph::default_config_t::kBusLinks, which carries
 * the full rationale, the measured saving and the refusal rule. Derived from
 * @ref tr::graph::config_t exactly as @ref tr::mem::kSpinWaitSafe is, so an override fragment
 * sets it in the one place every knob is set. Its consumers reach it through
 * `tr::net::bus_of` (`%transport.hpp`) rather than reading it directly.
 */
inline constexpr bool kBusLinks = tr::graph::config_t::kBusLinks;

/**
 * @brief Whether this target carries the RFC-0014 §4 S5 link-liveness engine at all.
 *
 * The transport plane's spelling of @ref tr::graph::default_config_t::kSelfHealLinks, which
 * carries the full rationale, the measured saving and the refusal rule. Derived from
 * @ref tr::graph::config_t exactly as @ref kBusLinks is, so an override fragment sets it in
 * the one place every knob is set.
 */
inline constexpr bool kSelfHealLinks = tr::graph::config_t::kSelfHealLinks;

/**
 * @brief Whether a connection SPEC may carry the dial-side `insecure` key.
 *
 * The transport plane's spelling of @ref tr::graph::default_config_t::kAllowInsecureTls, which
 * carries the rationale. Derived from @ref tr::graph::config_t exactly as @ref kBusLinks is,
 * so an override fragment sets it in the one place every knob is set. Its consumers are the
 * `quic` and `webtransport` factories.
 */
inline constexpr bool kAllowInsecureTls = tr::graph::config_t::kAllowInsecureTls;

/**
 * @brief Stack bytes for the link-liveness engine's worker thread; `0` = platform default.
 *
 * The transport plane's spelling of @ref tr::graph::default_config_t::kSelfHealWorkerStackBytes,
 * which carries the rationale. Its one consumer is the `pthread_attr_setstacksize` at the
 * worker spawn (`self_heal_link.cpp`).
 */
inline constexpr std::size_t kSelfHealWorkerStackBytes =
    tr::graph::config_t::kSelfHealWorkerStackBytes;

}  // namespace tr::net
