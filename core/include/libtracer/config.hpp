/**
 * @file
 * @brief Per-target build configuration as plain C++ (ADR-0068): every compile-time
 *        knob is an `inline constexpr` constant or a `using` policy binding — never a
 *        preprocessor definition.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two renderings of one template exist:
 *
 *   - `config.hpp.in` — the single source of truth. CMake `configure_file`s it into the
 *     build tree with the target's knob values and prepends that directory to the PUBLIC
 *     include path, so the generated file SHADOWS this one for every TU of the build
 *     (one file per build ⇒ no per-TU `-D` mismatch, no ODR hazard).
 *   - the checked-in `%config.hpp` — the default rendering, kept byte-identical by a
 *     configure-time drift gate (`FATAL_ERROR` on mismatch), so a raw `-Icore/include`
 *     consumer (the Cortex-M0 footprint gate, vendored source drops) builds with stock
 *     settings and no build-system participation.
 *
 * Adding a knob: add it HERE (both renderings via the gate), never as a macro in a
 * public header. The CMake cache variable / Kconfig option is the user-facing name; this
 * header is its C++ delivery vehicle.
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
 * **Declaring your own.** Copy this struct, change what differs, and bind it — one alias,
 * app-wide, in the header your build puts ahead of this one on the include path:
 *
 * ```cpp
 * struct my_node_config_t : tr::graph::default_config_t {
 *     static constexpr std::size_t kCacheLineBytes = 0;  // single-core: no false sharing
 *     using lkv_slot_t = tr::graph::sp_atomic_slot_t;
 * };
 * using config_t = my_node_config_t;
 * ```
 *
 * Inheriting from @ref default_config_t means a knob added later does not break your preset —
 * it inherits the new default instead of failing to compile.
 */
struct default_config_t {
    /**
     * @brief The number of lock stripes shared by every vertex in the process (#361 §2).
     *
     * CMake: `-DLIBTRACER_VERTEX_LOCK_STRIPES=8`; ESP-IDF: menuconfig
     * `CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES`. A small single-core node reclaims RAM at 4-8.
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
     * CMake: `-DLIBTRACER_CACHE_LINE_BYTES=0`. The ESP-IDF component derives it from
     * `CONFIG_FREERTOS_UNICORE` — a unicore build has no second core by construction, so the
     * right value is not a question the integrator should be asked.
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
     * knob rather than a thread ceiling picked out of the air (RFC-0006); CMake:
     * `-DLIBTRACER_HAZARD_READER_SLOTS=24`.
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
     * (RFC-0006); CMake: `-DLIBTRACER_EDGE_PIN_SLOTS=24`.
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
     * twenty-four readers (ADR-0069 §6 — the real path, not the model bench's 20.8x). CMake:
     * `-DLIBTRACER_LKV_SLOT=<type>`. The named type must satisfy the policy contract in
     * `%lkv_slot.hpp` — in particular `load()` returns an OWNING handle.
     */
    using lkv_slot_t = sp_atomic_slot_t;
};

/**
 * @brief THE configuration this build uses — the one binding, and the one thing to override.
 *
 * An application selects its configuration by making this alias name its own traits type,
 * app-wide, exactly once. Everything below is derived from it, so nothing else has to change.
 */
using config_t = default_config_t;

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
/** @brief @ref default_config_t::acl_policy_t for this build. */
using acl_policy_t = config_t::acl_policy_t;
/** @brief @ref default_config_t::lkv_slot_t for this build. */
using lkv_slot_t = config_t::lkv_slot_t;

}  // namespace tr::graph

// ---------------------------------------------------------------------------------------------
// L0 (tr::mem) build configuration. Delivered by the same header for the same reason (ADR-0068:
// one file per build, so every TU agrees); a separate namespace because the fact it states
// belongs to the memory layer, not the graph.

namespace tr::mem {

/**
 * @brief Whether a task on this target may SPIN-WAIT for a lock another task holds.
 *
 * True on a multi-core host: the holder is running on a different core, so a spinner makes
 * progress possible and the O(1) section costs less than a mutex round-trip. FALSE on a
 * priority-preemptive scheduler, where a spinner that outranks the holder never yields the
 * CPU the holder needs to release the lock — the wait becomes unbounded priority inversion
 * and the board hangs in the watchdog rather than merely running slowly. That is true of a
 * single-core chip and equally of an SMP chip whose spinner and holder share a core.
 *
 * A target fact, so the BUILD sets it and nothing asks the integrator (the same reasoning
 * that derives @ref tr::graph::kCacheLineBytes rather than exposing it). Its one consumer
 * is the guard in `synchronized_pool_t`, which refuses to instantiate the spinlock policy
 * where spin-waiting is unsafe.
 */
inline constexpr bool kSpinWaitSafe = true;

}  // namespace tr::mem
