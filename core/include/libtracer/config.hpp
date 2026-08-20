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

namespace tr::graph {

/**
 * @brief The reserved @ref default_config_t::kPinPayloadRatio sentinel: never pin, always take
 *        the ADR-0041 §2 one-copy store (RFC-0022 §3.D).
 */
inline constexpr std::uint32_t kPinNever = 0;

struct allow_only_policy_t;  // security_acl.hpp — the ALLOW-only MCU profile (ADR-0020 subset)
struct full_acl_policy_t;    // security_acl.hpp — ordered first-match-per-bit with DENY
class sp_atomic_slot_t;      // lkv_slot.hpp — atomic<shared_ptr>; reclamation is the refcount
class hazard_slot_t;         // lkv_slot.hpp — lock-free atomic<node*>; hazard-pointer reclamation
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
 *     using lkv_slot_t = sp_atomic_slot_t;
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
     * to `hazard_slot_t` — the default binding never references the registry, so it is never
     * emitted. Undersizing is not a correctness problem: threads past the bound share one
     * reserved index under a spin lock, so they serialize with each other and with nobody else.
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
     * History, newest first: 96 (measured across all three CI legs — `acl_full` OFF/ON and
     * both `lkv_slot_t` bindings agree), 112 post-#380 §1, 144 post-packing, 168 post-§3,
     * 160 post-§2, 248 post-§1, 536 pre-split.
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
    static constexpr std::size_t kMaxVertexBytes64 = 96;

    /**
     * @brief The RAM-diet RATCHET on `sizeof(vertex_t)`, 32-bit (MCU) targets.
     *
     * Pointer-halved, and pinned to the measurement on the same terms as
     * @ref kMaxVertexBytes64 — measured 72 B on rv32 (`-Os -fno-exceptions -fno-rtti`,
     * `rv32imac_zicsr_zifencei`/`ilp32`), identical across all three configuration legs.
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
    static constexpr std::size_t kMaxVertexBytes32 = 72;

    /**
     * @brief The RFC-0022 §3.D pin/copy amplification ratio — pin the written value as a subview
     *        of the inbound frame iff `payload_bytes * K >= segment_bytes` (and the payload is
     *        trailer-less).
     *
     * `K` is not a synthetic limit: both branches are correct and `K` selects which correct
     * branch is cheaper. Pinning holds the whole owning RX **segment** for the value's lifetime,
     * so `K` bounds the waste at `(K-1)x` the payload where an absolute byte threshold bounded
     * it not at all.
     *
     * `segment_bytes` is the ALLOCATED size of the segment the pin would keep alive, not the
     * length of the delivered frame view. Those differ by a lot on a real transport:
     * `udp_transport_t` receives every datagram into a `kMaxDatagram`-sized segment and delivers
     * a length-`n` window over it, so a 1 KB datagram pins 64 KB. Measuring the view length instead
     * would price a cost nobody pays and ignore the one everybody does.
     *
     * @ref kPinNever (0) is the reserved sentinel: never pin. It is the value shipped here, and
     * it reproduces the pre-RFC default behaviour exactly (the old absolute threshold defaulted
     * to 0 and its predicate required `> 0`). RFC-0022 §8 Q3 was answered by §6's measurement
     * (Amendment 2, PR #771): the sentinel is the landing default on BOTH targets — the
     * on-by-default flip does not land.
     *
     * @section pin_borrow The borrow is APP-OWNED policy, and this constant is the decision surface
     *
     * A pinned value **borrows** its inbound RX segment for its whole lifetime — not for the
     * delivery window, for as long as the value is the vertex's last-known value. On a pooled RX
     * backend that borrow is a **pool slot**, i.e. receive capacity, unavailable to the transport
     * until the value is displaced or the vertex dies. The library makes the deferred release
     * *safe* — atomic segment refcounts, so a borrow outliving the recv frame is never a
     * use-after-free — but nothing in the library bounds the *budget*, and nothing can: only the
     * application knows its pool geometry and its retention pattern.
     *
     * So the quantity to size against is `live pinned values x segment_bytes`. `K` bounds the
     * waste per value; it does **not** bound the number of values, which is why no `K` is a
     * remedy for a retain-heavy workload — measured, see `bench/README.md` §"RFC-0022 §6 —
     * receive-pool occupancy": at the ESP32-C6 RX geometry every `K` that pins at all collapsed a
     * 29-slot pool identically once the live vertex count crossed the slot count.
     *
     * Target-class guidance, and the reason the shipped value is the sentinel on both:
     * - **NARROW** — set the sentinel. A fixed, small RX pool cannot fund an indefinite borrow;
     *   the same off-by-default-on-NARROW posture as the RFC-0027 label table.
     * - **WIDE / MID** — may borrow freely; the pool is large relative to the retained set, and
     *   the borrow is the zero-copy latency win.
     */
    static constexpr std::uint32_t kPinPayloadRatio = 0;

    /**
     * @brief The target's selected ACL policy (ADR-0047 §1 build-time module set).
     *
     * Default: the ALLOW-only MCU profile. The CMake option `LIBTRACER_ACL_FULL=ON` rebinds
     * this to the full `security_acl` host policy (ordered first-match-per-bit with DENY) — a
     * target-configuration change, never an edit to `graph.cpp`.
     */
    using acl_policy_t = allow_only_policy_t;

    /**
     * @brief The target's selected LKV slot policy (ADR-0069 §1).
     *
     * How a vertex publishes and reads its last-known value. Default: `sp_atomic_slot_t`, the
     * `std::atomic<std::shared_ptr<const rope_t>>` libtracer has always used, whose reclamation
     * is the refcount and whose registry cost is zero — the right choice for a write-dominated
     * single-core node, and the reason a raw `-I` consumer builds what it always built.
     *
     * A many-core host is the case for rebinding this: today's slot INVERTS under concurrent
     * readers, and a reclamation scheme that does not serialize recovers roughly 4x of that at
     * twenty-four readers (ADR-0069 §6 — the real path, not the model bench's 20.8x). Override
     * fragment: `using lkv_slot_t = hazard_slot_t;`. The named type must satisfy the contract in
     * `%lkv_slot.hpp` — in particular `load()` returns an OWNING handle.
     */
    using lkv_slot_t = sp_atomic_slot_t;

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
     */
    static constexpr bool kSpinWaitSafe = true;

    /**
     * @brief Whether this target's memory model may REORDER a later relaxed load ahead of an
     *        earlier `seq_cst` store — i.e. whether it is anything WEAKER than x86-64's TSO
     *        (#1143).
     *
     * The one knob here that is not a size, a policy or a preference: it states what the
     * hardware does, so that an ordering precondition can be a `static_assert` instead of a
     * paragraph. The precondition it carries today is the delivery-skip Dekker pair (#635,
     * #1140) — `vertex_t::own_subs_ordered` against ADR-0049's subscribe latch — whose
     * `seq_cst` halves are argued from the code rather than from coverage, because a relaxed
     * ablation leaves the whole suite green wherever CI happens to run.
     * `kDeliverySkipOrder` (`vertex.hpp`) is the constant this refuses to see weakened.
     *
     * **Default `true`: assume weak unless a target says otherwise.** The two directions are
     * not symmetric. Saying `true` on a TSO host costs exactly nothing — the orders this gates
     * are already `seq_cst` on every target, so the assertion is satisfied as shipped and no
     * instruction changes. Saying `false` on a target that is actually weak silently disarms
     * the check on the one class of target it exists for, and the shipped set is mostly that
     * class: rv32 (esp32c6/c3), Cortex-M0, and the `ubuntu-24.04-arm` CI leg (#1140) are all
     * weakly ordered, and a raw `-I` consumer — a vendored source drop, the footprint gate —
     * states nothing at all. The value that is safe to inherit in silence is therefore the
     * strict one.
     *
     * It never SELECTS a weaker order: nothing reads this to relax an access, so a target that
     * sets it `false` gets the same instructions, only a check that stops firing. Override
     * fragment: `static constexpr bool kWeaklyOrdered = false;` — worth setting only for an
     * x86-64-only build that wants the freedom to relax those loads, which is a decision to
     * take deliberately rather than by omission.
     */
    static constexpr bool kWeaklyOrdered = true;
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
/** @brief @ref default_config_t::kPinPayloadRatio for this build. */
inline constexpr std::uint32_t kPinPayloadRatio = config_t::kPinPayloadRatio;
/** @brief @ref default_config_t::kDeferredReleaseSlots for this build. */
inline constexpr std::size_t kDeferredReleaseSlots = config_t::kDeferredReleaseSlots;
/** @brief @ref default_config_t::kQsbrParticipants for this build. */
inline constexpr std::size_t kQsbrParticipants = config_t::kQsbrParticipants;
/** @brief @ref default_config_t::kWeaklyOrdered for this build. */
inline constexpr bool kWeaklyOrdered = config_t::kWeaklyOrdered;
/** @brief @ref default_config_t::acl_policy_t for this build. */
using acl_policy_t = config_t::acl_policy_t;
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
