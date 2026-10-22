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

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace tr {

template <std::size_t LineBytes, std::size_t Stripes>
struct basic_mutex_guard_t;  // guard_mutex.hpp — the host guard: address-striped locks (hosted)
using mutex_guard_t = basic_mutex_guard_t<64, 64>;  // its default sizing (guard_mutex.hpp)
struct no_guard_t;           // guard.hpp — guards nothing; single-threaded builds only
struct stderr_fault_sink_t;  // init_fault.hpp — the host default: one line on stderr

}  // namespace tr

namespace tr::graph {

/**
 * @brief What an owner declares about storing a written value by reference or by copy
 *        (RFC-0022 Amendment 3): a declaration, not a number.
 *
 * Whether a stored value SHARES its inbound receive block (refcount, zero copy) or is COPIED
 * into a block of its own is the build's block source's answer, given by
 * @ref tr::mem::share_fits. The static arena and the heap root always share, and the slab pool
 * shares on a class fit, when a copy would draw a block no smaller than the one already held.
 * A vertex states only what its value is for. Owner-side and host-only, like
 * @ref retention_t, and no peer reads or writes it.
 *
 * Bytes the receive path does not own (a borrowed span) and a value whose TLV carries a CRC or
 * TS trailer are copied whatever the hint. A build states the hint an undeclared vertex answers
 * with @ref default_config_t::kDefaultShareHint.
 */
enum class share_hint_t : std::uint8_t {
    DEFAULT = 0, /**< @brief Follow the build: @ref default_config_t::kDefaultShareHint, whose
                  *          own default follows the block source's trait. */
    SHARE = 1,   /**< @brief Share whenever the value sits in a shareable block. */
    COPY = 2,    /**< @brief Always copy: the owner's lever for a long-lived value that would
                  *          otherwise hold a receive block for its whole lifetime. */
};

/**
 * @brief A size-class ladder: @p PerDoubling classes of @p Step bytes from @p Step, then
 *        @p PerDoubling evenly spaced classes in every doubling up to @p Top, then, when
 *        @p Header is not 0, one more class of `Top + Header` bytes.
 *
 * The generator behind @ref default_config_t::kSizeClasses (#1777), so the default table is
 * spelled by its parameters rather than as 81 literals. @p Header is for a table whose largest
 * PAYLOAD is @p Top: the one-block segment carrying it is a segment header past @p Top, and
 * without the extra row it would fall past the last class (#1990). It must be a multiple of
 * @p Step, like every other row.
 */
template <std::size_t Step, std::size_t PerDoubling, std::size_t Top, std::size_t Header = 0>
struct size_class_ladder_t {
    static_assert(Header % Step == 0, "the header row keeps the ladder's step");
    /** @brief Rows in the ladder. */
    static constexpr std::size_t kRows = [] {
        std::size_t n = PerDoubling + (Header != 0 ? 1 : 0);
        for (std::size_t lo = Step * PerDoubling; lo < Top; lo *= 2) n += PerDoubling;
        return n;
    }();
    /** @brief The ladder, ascending. */
    static constexpr std::array<std::size_t, kRows> kTable = [] {
        std::array<std::size_t, kRows> t{};
        std::size_t i = 0;
        for (std::size_t k = 1; k <= PerDoubling; ++k) t[i++] = k * Step;
        for (std::size_t lo = Step * PerDoubling; lo < Top; lo *= 2)
            for (std::size_t k = 1; k <= PerDoubling; ++k) t[i++] = lo + k * (lo / PerDoubling);
        if (Header != 0) t[i] = Top + Header;
        return t;
    }();

    /**
     * @brief The smallest row of @ref kTable that holds @p bytes, or @p bytes itself above the
     *        last row: the table's own answer by arithmetic, with no table read and no search
     *        (#2143).
     *
     * The ladder is geometric, so within a doubling `(lo, 2 lo]` the rows are the multiples of
     * `lo / PerDoubling`, and the answer is @p bytes rounded up to that step. Every term is
     * computed and the answer chosen by mask selects: no branch.
     */
    [[nodiscard]] static constexpr std::size_t ceiling(std::size_t bytes) noexcept {
        constexpr std::size_t kBase = Step * PerDoubling;
        static_assert(std::has_single_bit(Step) && std::has_single_bit(PerDoubling) &&
                          Top % kBase == 0 && std::has_single_bit(Top / kBase),
                      "the arithmetic ceiling needs a power-of-two step, rows per doubling and "
                      "doublings");
        const std::size_t small = ((bytes > Step ? bytes : Step) + Step - 1) & ~(Step - 1);
        // `x | 1` is never 0, so the bit width needs no zero test.
        const std::size_t x = (bytes > kBase ? bytes : kBase + 1) - 1;
        const std::size_t lo = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1 -
                                                  static_cast<unsigned>(std::countl_zero(x | 1)));
        const std::size_t step = lo / PerDoubling;
        const std::size_t geometric = (bytes + step - 1) & ~(step - 1);
        const std::size_t past =
            Header != 0 && bytes <= Top + Header ? std::size_t{Top + Header} : bytes;
        // Mask selects, not ternaries, so a compiler that would branch on the costlier arm
        // cannot.
        const auto pick = [](bool c, std::size_t a, std::size_t b) {
            return b ^ ((a ^ b) & (std::size_t{0} - static_cast<std::size_t>(c)));
        };
        return pick(bytes <= kBase, small, pick(bytes <= Top, geometric, past));
    }
};

struct allow_only_policy_t;  // security_acl.hpp — the ALLOW-only MCU profile (ADR-0020 subset)
struct full_acl_policy_t;    // security_acl.hpp — ordered first-match-per-bit with DENY
class hazard_slot_t;         // lkv_slot.hpp — lock-free atomic<node*>; hazard-pointer reclamation
class single_writer_slot_t;  // lkv_slot.hpp — one value_t* swapped inside guard_t; no spin
struct reclaim_strict_t;     // reclaim.hpp — grace point: `unsubscribe()` returns
struct reclaim_local_t;      // WITHDRAWN (#2127): declared, never defined, so naming it is refused
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
     * @brief How many 256-entry tables the portable CRC-32C fold reads per step: **8**
     *        (slice-by-8, 8 KiB of `.rodata`) or **1** (the byte-at-a-time Sarwate loop over
     *        the 1 KiB base table the constant-evaluated CRC already uses).
     *
     * Only the no-hardware path reads it: x86 with SSE4.2 and ARMv8 with the CRC extension
     * fold in the instruction and never touch either table. Every value produces the same
     * CRC, byte for byte (a frozen-vector requirement), so this is an optimization choice,
     * never a correctness one.
     *
     * The default is 8 on every target: a MID or WIDE node with no CRC instruction (an rv32
     * ESP32-C6, a 32-bit Linux board) keeps the fast fold on every frame. A NARROW node binds
     * 1 explicitly; on the Cortex-M0 P0 node the seven extra tables were 7,168 B of its 16 KiB
     * flash budget (#2116), and the footprint sentinel's fragment pins that choice.
     * The trade, indicative only (one isolated run of the portable fold under `nice`, host
     * x86-64, GCC 13.3, `-O2`): slice-by-8 folds 1.1 / 2.3 / 2.2 GB/s at 64 B / 1 KiB / 16 KiB,
     * the single table 0.33 / 0.40 / 0.44 GB/s, about 3.4x to 5.6x slower.
     * Override fragment: `static constexpr std::size_t kCrc32cSlices = 1;`.
     */
    static constexpr std::size_t kCrc32cSlices = 8;

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
     * @brief Subscribers a publish snapshots into its stack buffer before it falls back to the
     *        overflow vector (#1708) — a TUNING knob, never a limit.
     *
     * A publish copies the vertex's live edges into a fixed buffer of `edge_view_t` on its own
     * stack, so a fan-out up to this width reaches no allocator. A wider fan-out snapshots into
     * a frame of eight times this width on the same stack, and past that into a block from the
     * graph's table source (#1885), and delivers to every subscriber exactly as before: the
     * width selects a strategy, it refuses nothing and reports nothing. Only a table source
     * that refuses the block sheds the tail, as a counted `fan_out_truncated` drop.
     *
     * The buffer is reserved on EVERY publish frame whether the vertex has one subscriber or
     * eight, at `kInlineFanout * sizeof(edge_view_t)` bytes: 384 B on a 64-bit host (48 B a
     * view) and 224 B on rv32 (28 B). The wide fan-out's frame sits in the same stack frame at
     * eight times that (#1885): 3,072 B on a 64-bit host, 448 B on rv32 at 2. The
     * host default keeps the allocation-free fast path for the widths the benches gate; a
     * NARROW node whose vertices carry one or two subscribers spends that stack on every task
     * that publishes. Measured on rv32 after #1885 (`-Os`, GCC 15.2, real
     * `core/src/graph.cpp`, `-fstack-usage`, #1912): `graph_t::fan_out`'s frame (its outlined
     * body, `fan_out.part.0`) is 2,128 B at 8 and 608 B at 2. The difference, 1,520 B, is the
     * six inline views (168 B) plus the 48 views the wide frame loses (1,344 B) and 8 B of
     * frame rounding. Before #1885 it was 304 B at 8 and 128 B at 2. Override
     * fragment: `static constexpr std::size_t kInlineFanout = 2;` — the ESP-IDF component
     * sets 2 on a chip target. At least 1.
     */
    static constexpr std::size_t kInlineFanout = 8;

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
     * @brief The copy-or-share hint a vertex that declares none answers (RFC-0022 Amendment 3,
     *        #2226): a build-wide @ref share_hint_t.
     *
     * `DEFAULT`, the default, follows the build's block source (`tr::mem::share_fits`): the
     * static arena and the heap root share a trailer-less written value, the slab pool shares
     * it on a class fit. `COPY` makes copy the build's posture, so a long-lived value (a knob,
     * a setting) never holds a pooled receive block for its whole life and only the vertices
     * that declare `share_hint_t::SHARE` keep theirs; `SHARE` shares wherever the value sits in
     * one block. A vertex's own declaration wins over it, and one that declares `DEFAULT` gets
     * this value.
     *
     * Compile-time, like every member here: the read is one constant fold, so at `DEFAULT`
     * the write path compiles to what it was without the knob. Override fragment:
     * `static constexpr share_hint_t kDefaultShareHint = share_hint_t::COPY;`.
     */
    static constexpr share_hint_t kDefaultShareHint = share_hint_t::DEFAULT;

    /** @brief The ladder that generates the default @ref default_config_t::kSizeClasses. A
     *         classed root whose table is this one finds a class by its arithmetic `ceiling`
     *         (#2143). */
    using size_classes_t = size_class_ladder_t<16, 8, 65536, 48>;

    /**
     * @brief The size-class table of the host slab pool (ADR-0083 Decisions 4 and 5, #1775,
     *        #1777): the block sizes a class serves, ascending.
     *
     * The host default root (`%mem_slab_pool.hpp`, `tr::mem::host_root_t`) carves every block it
     * serves from a slab of one class: a request takes the smallest class that holds it, and
     * the host allocator sees only whole slabs. A size-class boundary is therefore a row in
     * this one table, not a property of the platform allocator. That is what removes the
     * 1 KiB cliff #1768 measured: a 1024 B value's 1072 B one-block segment is a 1152 B block
     * here, from the same per-thread cache a 1000 B one comes from, where glibc served it on a
     * slower path than a 1032 B request.
     *
     * **Default: 16 B to 64 KiB, 8 classes per doubling, plus one for a 64 KiB payload's
     * segment.** Eight classes of 16 B up to 128 B, then eight evenly spaced classes in every
     * doubling above it (144 ... 256, 288 ... 512, ... 61440, 65536), so no block wastes more
     * than one eighth of its size; then 65584, a 64 KiB payload with the 48 B header of its
     * one-block segment on a 64-bit host (#1990), where it would otherwise have fallen to the
     * root at about twice the cost of a classed draw. 81 rows. A class takes no memory until its
     * first request, so the extra row costs a node that never holds a 64 KiB value only its
     * table entry. A request above the last row, or one aligned past 64 B, is drawn from the
     * root as whole slabs of its own (@ref kSlabBytes multiples) and returned there when freed.
     *
     * Every row must be a multiple of 16 (`alignof(std::max_align_t)` on the reference hosts),
     * so every block keeps the platform allocator's alignment, and the table must be non-empty
     * and strictly ascending, with at most 255 rows; the pool asserts all three. A node whose
     * payloads cluster (mostly 4 KiB frames) states its own table, and pays no slab for a class
     * it never draws: a class takes its first slab on its first request.
     *
     * Where @ref kSlabPool is `false` the same table sizes the static arena's classes instead
     * (@ref kArenaBytes): a block takes the smallest row that holds it there too, from that
     * row's free list.
     *
     * Override fragment: `static constexpr std::size_t kSizeClasses[] = {64, 256, 1024, 4096};`.
     */
    static constexpr auto kSizeClasses = size_classes_t::kTable;

    /**
     * @brief Whether this build's default root is the host slab pool (ADR-0083 Decision 4,
     *        #1777).
     *
     * `true` (the host default): a `graph_t` built without a source derives its value, table
     * and net sub-pools from `tr::mem::host_root()`, and `tr::mem::heap_backend()`,
     * `value_source()`, `table_source()`, `net_source()` and `net_backend()` all draw from them.
     * The host allocator then serves whole slabs only (@ref kSlabBytes), never a request per
     * value.
     *
     * `false` (the MCU default, ADR-0083 Decision 4, #1783): the default root is a STATIC
     * ARENA of @ref kArenaBytes (`%mem_arena.hpp`, `tr::mem::arena_root_t`), carved into the
     * classes of @ref default_config_t::kSizeClasses, and the same five defaults draw from its
     * value, table and net sub-pools. No byte of the default root comes from a heap; the arena
     * never trims, and an exhausted arena refuses (BACKPRESSURE at run time, a sizing abort at
     * init). A target with a few hundred KiB of RAM gains nothing from 64 KiB slabs, so the ESP-IDF
     * component binds `false`. Until v0.19.0 this setting meant the platform heap; an application
     * that still wants that injects `tr::mem::heap_source()` into its `graph_t`.
     *
     * Override fragment: `static constexpr bool kSlabPool = false;`.
     */
    static constexpr bool kSlabPool = true;

    /**
     * @brief The static arena's size in bytes, where @ref kSlabPool is `false` (ADR-0083
     *        Decision 4, #1783).
     *
     * The MCU default root is one array of this many bytes in `.bss`, so the linker map shows
     * exactly what the library holds by default. Every sub-pool carves from it, and nothing
     * it carves goes back to it: a block returned to a class is reused by that class only.
     * Size it against the node's peak per class (`:stats.mem.values`, `.tables` and `.net`
     * report each sub-pool's high-water mark, and the root's census the bytes carved). An
     * application that injects its own root into every `graph_t` can bind a small value; the
     * defaults that take no graph (`tr::mem::value_source()` and its siblings) still draw here.
     * With every source injected (the graph's root, the router's planes, the transport
     * vertex's rx and egress stores), a long vertex name, a connection's liveness value, a
     * bus link's `:children[]` listing and a composed field read's value block draw nothing
     * here (#2052): the arena's minimum is 0 B. Measured on a bus connection created,
     * published UP and listed with two peers: 288 B carved before #2052, 0 B after, on
     * Cortex-M0 and on ESP32-C6.
     *
     * **0 opts out of the arena (#2090): the platform heap becomes the default root.** No
     * region, no free-list heads and no root object are compiled, so the default root costs
     * no static byte, and `tr::mem::default_root()`, `value_source()`, `table_source()` and
     * `net_source()` are `tr::mem::heap_source()`, `heap_backend()` and `net_backend()` the
     * platform heap. The trade-off: every default the application has not redirected draws
     * from the heap, unbounded, and the no-heap link check (ADR-0083 Decision 1) does not hold
     * for that image; it is a deliberate exception for an application that injects its own
     * sources everywhere. A nonzero value is at least the smallest row of
     * @ref default_config_t::kSizeClasses.
     *
     * The free-list heads are sized from the region: one per sub-pool per row of
     * @ref default_config_t::kSizeClasses no larger than this (`%tr::mem::kArenaClasses`), since a
     * larger class could never be carved.
     *
     * Not read where @ref kSlabPool is `true`: no arena is compiled there.
     *
     * Override fragment: `static constexpr std::size_t kArenaBytes = 65536;`, or `= 0;` for
     * the heap-rooted build.
     */
    static constexpr std::size_t kArenaBytes = 32768;

    /**
     * @brief The slab the host slab pool draws for a class, in bytes: the smallest unit it asks
     *        the root for (#1777). A power of two.
     *
     * A class draws a slab of this size, or the next power of two that holds 8 of its blocks if
     * that is larger, aligned to its own size, so a block finds its slab by masking its
     * address. A slab is carved lazily, one block at a time, so its untouched pages cost the
     * node address space and not resident memory. Default 64 KiB: one slab holds 8 or more
     * blocks of every class up to 7 KiB.
     *
     * Override fragment: `static constexpr std::size_t kSlabBytes = 16384;`.
     */
    static constexpr std::size_t kSlabBytes = 65536;

    /**
     * @brief The fully free slabs a host slab-pool class keeps (ADR-0083 Decision 6, #1777).
     *
     * A slab whose last block comes back is RELEASED to the root when its class already keeps
     * this many fully free slabs, and kept for the next burst otherwise, however many slabs of
     * the class are live, so a long-running service
     * gives back what a burst took instead of holding its peak forever. Nothing is released on
     * a timer (there is none in the library): `tr::mem::host_root_t::trim()` releases every
     * fully free slab on the application's own schedule. At least 1, so a class that
     * alternates one allocation and one free does not draw and release a slab each time.
     *
     * Override fragment: `static constexpr std::size_t kSlabClassCap = 1;`.
     */
    static constexpr std::size_t kSlabClassCap = 2;

    /**
     * @brief The target's selected ACL policy (ADR-0047 §1 build-time module set).
     *
     * Default: the ALLOW-only MCU profile. A host binds the full `security_acl` policy
     * (ordered first-match-per-bit with DENY) in its override fragment — a
     * target-configuration change, never an edit to `graph.cpp`. Override fragment:
     * `using acl_policy_t = full_acl_policy_t;`. (The `-DLIBTRACER_ACL_FULL` CMake option that
     * used to write that line was removed in #1722; configuring with it is an error.)
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
     * @brief Where core reports a setup-time sizing fault before it aborts (#1885).
     *
     * Core has one such report: a memory source or the vertex ceiling refusing a call that
     * initialization cannot do without (ADR-0056 amendment, ADR-0083). It hands the facts
     * (`tr::init_fault_t`) to `fault_sink_t::report` and aborts when that returns, so core
     * itself writes to no stream. The host default prints one line on `stderr`; a target with
     * no stdio binds `tr::silent_fault_sink_t` or its own type with the same static `report`.
     * Override fragment: `using fault_sink_t = my_uart_fault_sink_t;`.
     */
    using fault_sink_t = ::tr::stderr_fault_sink_t;

    /**
     * @brief Locks in the host guard's process-wide stripe table (#1716); a power of two.
     *
     * Applies when the build keeps the default @ref guard_t, and then the library then binds
     * `tr::basic_mutex_guard_t<kCacheLineBytes, kGuardStripes>`, so the table costs
     * `kGuardStripes * max(kCacheLineBytes, 1)` bytes of `.bss` — 4 KB at the defaults. Two
     * vertices share a lock only when their addresses hash to one stripe, and a shared stripe
     * costs a contended section, never correctness. Override fragment:
     * `static constexpr std::size_t kGuardStripes = 16;`. Ignored when @ref guard_t is bound to
     * anything else (an interrupt-masked guard has no table).
     */
    static constexpr std::size_t kGuardStripes = 64;

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
     * @brief Force the guarded binding of every `%tr::graph::bound_rmw_counter_t` the library
     *        owns — the write sequence and the router's labelled-hop count — even where the
     *        counter's width is natively lock-free (#1715, #1697).
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
     * Default, on every target: `reclaim_qsbr_t`, whose grace point is "every dispatching
     * thread has passed a quiescent state" (ADR-0080 Amendment 1 §1, #2127). A retired pair is
     * released inline when no participant is mid-dispatch (the ordinary case) and otherwise by
     * whichever participant completes the grace period, so the release hook may run on a thread
     * other than the `unsubscribe()` caller and must be short, thread-safe and non-blocking —
     * see `%reclaim.hpp`. It prices one relaxed load, one release store and one fence per
     * OUTERMOST bracket (not per edge; the fence is a compiler barrier under `kSingleCore`).
     *
     * The one opt-out: `using reclaim_policy_t = reclaim_strict_t;`, which tracks nothing and
     * releases inline before `unsubscribe()` returns. Take it only where the deployment can
     * show that no dispatch is live when it unsubscribes — on any thread — because
     * `reclaim_strict_t` cannot detect it. `reclaim_local_t` is withdrawn: a fragment that
     * still names it is refused with the migration note.
     */
    using reclaim_policy_t = reclaim_qsbr_t;

    /**
     * @brief How many retired `{ctx, release}` pairs the QSBR domain may hold at once, waiting
     *        for a grace period (ADR-0080).
     *
     * The pairs are held across a cross-thread GRACE PERIOD in ONE shared table, so the quantity
     * this bounds is "retirements in flight process-wide while some participant has yet to
     * quiesce". The ordinary unsubscribe, with no participant mid-dispatch, holds nothing: its
     * hook runs inline. Raise it (64 is a sane starting point) on any node that unsubscribes in
     * bulk while other threads dispatch. Nothing at all under `reclaim_strict_t`, which never
     * defers.
     *
     * Sizing: each slot is a handoff word, the `{ctx, release}` pair and its epoch, in `.bss`,
     * with no destructor and no allocation: 32 B on a 64-bit host and 16 B on a 32-bit MCU, so
     * the default 16 slots cost 512 B and 256 B. Now that `reclaim_qsbr_t` is the default on
     * every target (#2127), size the table for the largest burst of unsubscribes that can
     * overlap another thread's dispatch, not for the steady state: one slot per subscription
     * a node may retire while some participant is still inside a fan-out. A single-threaded
     * node, or one that unsubscribes only while no other thread dispatches, retires inline and
     * never holds a slot. A node that tears down a whole subtree under live traffic wants the
     * subtree's subscription count here. Watch `graph_t::reclaim_refusals().release_refused`
     * in a soak: any non-zero reading means the bound is too small for that deployment.
     *
     * Overflow is a REFUSAL, not a failure: a pair that finds every slot taken is DROPPED and
     * its hook is never run — a leak, deliberately, because the alternative is running a
     * release hook while a fan-out that is still walking the snapshot names that context.
     * A full table is drained and scanned again before a pair is refused, so a drop means the
     * table is full of pairs some participant still holds: either one participant has not
     * reached a quiescent state, or a burst of retirements outran the table while participants
     * were still inside their fan-outs. The `unsubscribe` answers `BACKPRESSURE` and
     * `graph_t::reclaim_refusals()` counts every one (`release_refused`), so an undersized bound
     * is observable rather than silent. Override fragment:
     * `static constexpr std::size_t kDeferredReleaseSlots = 64;`
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
     * The derivation follows the config the build binds (#2111): a fragment that sets
     * @ref kEdgePinSlots and leaves this knob alone gets that many participants, not this
     * struct's 32. A member initializer here cannot see a derived struct's value, so the
     * namespace-scope `tr::graph::kQsbrParticipants` makes the choice, through
     * `%qsbr_participants_v`: the bound config's own `kQsbrParticipants` when it declares one,
     * else its `kEdgePinSlots`.
     *
     * The two sets are not quite the same: a transport's receive thread opens the router's
     * frame bracket, and so takes a cell, without ever taking an edge pin. A node with more
     * receive threads than publishing ones should declare this knob rather than rely on the
     * derivation.
     *
     * **Unlike @ref kEdgePinSlots, correctness is not indifferent to this number** — but it
     * fails SAFE, not silently. A thread that finds every index taken cannot announce itself,
     * and a thread a scan cannot see is one no grace period may conclude past; so it counts
     * itself into an overflow tally instead, and any non-zero reading blocks all reclamation
     * until it clears. The failure mode is therefore deferred frees (visible as
     * @ref tr::graph::graph_t::reclaim_refusals rising), never a use-after-free.
     *
     * Its `.bss` is `N * max(kCacheLineBytes, alignof(std::uint32_t))` bytes, the floor being
     * the 31-bit epoch's state word: at N = 32 that is 2,048 B on a host. The transport plane
     * uses the same cells in every build (`fwd_router_t` brackets each inbound frame and
     * origination with them), so a build with the router carries this `.bss` under either
     * policy; a thread that finds every index taken still fails safe, deferring the free of a
     * removed link. The retired table is a separate object that only `reclaim_qsbr_t` names, so
     * the linker drops it from a `reclaim_strict_t` build.
     * Override fragment:
     * `static constexpr std::size_t kQsbrParticipants = 64;`
     */
    static constexpr std::size_t kQsbrParticipants = kEdgePinSlots;

    /**
     * @brief Whether a frame bracket's entry announces itself with a plain store, leaving the
     *        full barrier to the reclaim side's `membarrier(2)` (the asymmetric fence).
     *
     * With it, `%detail_qsbr::enter` on a Linux host stores its cell with no fence; the reclaim
     * side, before a scan may conclude that a grace period has passed, makes every running
     * thread of the process execute a full barrier (`MEMBARRIER_CMD_PRIVATE_EXPEDITED`), so the
     * store-load ordering the entry's `seq_cst` fence gave is paid once per freeing scan, not
     * once per frame. The proof is in `qsbr.hpp`, section "The asymmetric fence". It applies only
     * where the target has `membarrier`: on any other target (ESP-IDF, Cortex-M0, any non-Linux
     * host) the entry stays the store and `seq_cst` fence, whatever this says. On Linux, a
     * process whose registration is refused (an older kernel, a seccomp filter) also keeps the
     * fenced entry.
     *
     * Off by default: the fenced entry costs about 2 ns per outermost bracket on an x86 host,
     * while every `membarrier` interrupts each running thread of the process and costs the
     * reclaimer tens of microseconds. A node that churns hooked subscriptions or removes links
     * pays far more on the reclaim side than it saves on frames (#2100 has the numbers). Opt
     * in on a read-mostly host node, where frames outnumber reclaim events by tens of thousands
     * to one. Under it, drain passes run single-flight: a drainer that finds another one
     * fencing skips its pass, which only defers a free.
     * Override fragment: `static constexpr bool kQsbrAsymmetricFence = true;`
     */
    static constexpr bool kQsbrAsymmetricFence = false;

    /**
     * @brief Whether this build runs on ONE hart: every thread that touches the library shares a
     *        single core, so the QSBR bracket's fences become compiler barriers.
     *
     * A **correctness fact, not a tuning knob**, and it fails safe to false. Under it the frame
     * bracket's entry and exit are plain stores with a compiler barrier (no `fence` on rv32, no
     * `dmb` on Cortex-M), and the reclaim side's fence is a compiler barrier too. The argument is
     * the single hart's (`qsbr.hpp`, section "The single-hart flavour"): one instruction stream,
     * and a context switch is a precise trap, so a descheduled task's accesses are all performed
     * before the next task's first instruction. Setting it on a build that runs on more than one
     * core removes fences the grace-period proof needs, which is a use-after-free.
     *
     * Each integration derives it from a build fact and never asks: the ESP-IDF component from
     * `CONFIG_FREERTOS_UNICORE` (on a chip target), a Zephyr integration from `!CONFIG_SMP`, a
     * bare-metal board preset by itself. It is never inferred from @ref kCacheLineBytes (a padding
     * knob an integrator may zero to save RAM) or from the target triple (a dual-core part shares
     * an architecture with a single-core one). It wins over @ref kQsbrAsymmetricFence, which then
     * has nothing left to save. ADR-0080 Amendment 1 §2 (#2126).
     * Override fragment: `static constexpr bool kSingleCore = true;`
     */
    static constexpr bool kSingleCore = false;

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
     * @brief The largest `max_frame` a UDP link receives on its thread's STACK when its sink
     *        borrows (#2142); `0` = never, the default.
     *
     * A borrowed-span UDP link (no rope receiver) receives each datagram into a stack array
     * of this many bytes plus one, so it holds no receive buffer at rest and pays one
     * `recvfrom` per datagram with no copy. A link whose `max_frame` is above it, and every
     * owning (rope) link, receives into one held segment of `max_frame + 1` bytes from its
     * `memory.rx` instead.
     *
     * It defaults to `0` so that a packaging nobody has audited fails safe: on an RTOS target
     * the stack is a real allocation per receive thread, and a scratch the task's stack cannot
     * hold is an overflow, not a cost. A POSIX host, where the stack is lazily committed
     * address space, opts in; this tree's host presets (the test and bench fragments) bind
     * 16384. A nonzero value makes a `udp_config_t::recv_stack` too small to hold the scratch a
     * construction-time refusal (`udp_transport_t::kMinRecvStack`). Override fragment:
     * `static constexpr std::size_t kUdpStackScratchBytes = 16384;`
     */
    static constexpr std::size_t kUdpStackScratchBytes = 0;

    /**
     * @brief Frames a link's receive context may consume back to back before it waits for
     *        its core to go idle once; `0` = no frame budget (ADR-0085).
     *
     * A receive context that always finds more bytes waiting never blocks, so on a core it
     * shares with the idle task a peer that keeps sending keeps that core busy for as long
     * as it likes. On ESP-IDF that is the IDLE task's watchdog: the board resets. The budget
     * bounds one DRAIN, which is the run of frames a receive context consumes without its
     * core going idle in between. When it is spent, the link stops reading and waits until
     * the core's idle task has run. The unread bytes stay in the socket, the TCP window
     * closes and the peer's own stack throttles it (ADR-0081 §2): no frame is dropped, none
     * is buffered by the library, and nothing measures time. The drain counter restarts
     * whenever the core idles on its own, so a link that is not saturated never waits.
     *
     * Its one reader is the ESP-IDF `httpd_ws_link_t`, whose receive context is the
     * `esp_http_server` task. Hosted links read the socket from their own thread under a
     * preemptive OS scheduler and do not consult it.
     *
     * **Default 32 — sized against the watchdog, not against throughput.** The longest
     * stretch the idle task can now be kept out is one budget of frames plus whatever
     * higher-priority work follows it. At the ~7.3 ms of board time per small request
     * measured on an ESP32-C6, 32 frames is about 0.23 s, under a quarter of the shortest task
     * watchdog IDF offers (1 s) and a twentieth of its default (5 s). Each wait costs the
     * time until the core idles, which on a link that is the only load is the time the
     * Wi-Fi and TCP/IP tasks need to settle; per 32 frames that is a small share of the
     * burst. A target whose frames are much dearer than that lowers it.
     *
     * **A non-zero budget requires that the core idles.** The wait ends only when the idle
     * task runs, and the idle task runs only once every other task on the core has blocked.
     * So after one budget of back-to-back frames the receive context yields to EVERY
     * lower-priority ready task on its core until all of them block: a priority inversion
     * that bounds the link's read rate under sustained load by the longest run of
     * lower-priority work. A build whose core may never idle (one that runs a low-priority
     * task that never blocks, and has turned the idle-task watchdog check off for it) must
     * bind `0` here and in @ref kRxDrainBytes, or the link stops reading for good after one
     * budget. The ESP-IDF component defaults both to `0` unless the task watchdog watches the
     * idle task of every core. Override fragment:
     * `static constexpr std::size_t kRxDrainFrames = 16;`
     */
    static constexpr std::size_t kRxDrainFrames = 32;

    /**
     * @brief Payload bytes a link's receive context may consume back to back before it waits
     *        for its core to go idle once; `0` = no byte budget (ADR-0085).
     *
     * The byte half of @ref kRxDrainFrames, with the same reader, the same wait and the same
     * reset. Frames are what a small-write flood costs; bytes are what a large-frame flood
     * costs, where every frame is copied and decoded. A drain ends when EITHER budget is
     * spent. The frame that crosses the line is read whole (a frame is never split), so one
     * drain consumes at most this many bytes plus one frame. The same requirement holds: a
     * non-zero value needs a core that idles (see @ref kRxDrainFrames).
     *
     * **Default 32,768** — the ESP-IDF link's own per-frame cap, so one maximum-size frame
     * is one drain, and about six default lwIP receive windows. Override fragment:
     * `static constexpr std::size_t kRxDrainBytes = 16384;`
     */
    static constexpr std::size_t kRxDrainBytes = 32768;

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
     * to nothing, and the accessors answer `0`. It also arms the RFC-0022 §6 pin/copy branch
     * counters of `%pin_instrument.hpp`, which were the `LIBTRACER_PIN_INSTRUMENT` macro until
     * #1722. A compile-time member rather than a
     * macro for ADR-0068's reason: a macro can differ per TU, and these counters change
     * `graph_t`'s layout.
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
     * @brief Whether the TEST-ONLY fault-injection hooks are compiled in (#1719):
     *        `%tr::detail::probe_fail_hook`, `%tr::detail::write_fault_inject_hook`,
     *        `%tr::net::detail::ws_peer_published_hook` and
     *        `%tr::net::detail::tcp_peer_publishing_hook`.
     *
     * Each hook is a process-wide function pointer that a test arms to drive a path a healthy
     * host never takes: a nothrow draw the heap refuses, a write errno glibc never returns, a
     * handshake instant held open. Nothing in a shipped node ever arms one, yet each check it
     * guards was a load and a branch on that node — the `probe_fail_hook` one on EVERY draw
     * from the process-default block source and every nothrow growth.
     *
     * **Default `false` — the lean choice.** Closed out, every check is an `if constexpr`
     * branch that compiles to nothing: the default allocation path carries no hook load, and
     * no hook variable is odr-used, so none is emitted into the library. The declarations stay
     * visible either way, so a test that arms a hook still compiles under a fragment that
     * leaves this `false`; arming one there simply changes nothing.
     *
     * **Who sets it.** The core test build, through its checked-in preset fragment
     * `core/tests/instrumented/libtracer/config_override.hpp`, the fragment `core/CMakeLists.txt`
     * writes for the deprecated `-D` knobs when that same test build passes them, and every CI
     * leg that binds a fragment of its own and runs the fault-injection tests. Override fragment:
     * `static constexpr bool kFaultInjection = true;`
     */
    static constexpr bool kFaultInjection = false;

    /**
     * @brief Whether a vertex can carry a CREATION HOOK (RFC-0030 §7.2): app logic on a parent
     *        that decides whether a data write to a missing child creates it.
     *
     * A data write whose walk misses answers `tr::path::not_found` and creates nothing, whatever
     * its origin (RFC-0030 §7.1): the local host API, a `FWD{WRITE}` terminus, or a branch
     * write's landing site. The hook is the one way back to "write creates", and only below a
     * parent whose owner installed one with `graph_t::set_creation_hook`.
     *
     * **Default `false` on every profile — the lean choice** (RFC-0030 §18 Q5). Closed out, no
     * vertex has a hook slot, `graph_t::set_creation_hook` answers `SCHEMA_NOT_FOUND`, and every
     * miss refuses: the miss arm compiles to the `not_found` answer and draws nothing.
     *
     * **Who sets it.** An application that wants opt-in creation, for example an OTA-style flow
     * where a write names the image it brings. The core test build sets it in its preset
     * fragment `core/tests/instrumented/libtracer/config_override.hpp`. Override fragment:
     * `static constexpr bool kCreationHooks = true;`
     */
    static constexpr bool kCreationHooks = false;

    /**
     * @brief Whether the graph carries the SESSION-ADMISSION seam (#1841):
     *        `graph_hooks_t::session_admission`, an embedder's per-session subscribe check and
     *        its per-session teardown event.
     *
     * With it, every subscription admitted for a session (a remote subscribe, keyed by the link
     * the departure evicts it by) is first offered to the installed hook, which may refuse it
     * with `BACKPRESSURE` before the edge or its departure-index entry exists, and the hook is
     * told once when that session's edges are torn down. That is what an embedder needs to hold
     * each session to its own subscription budget; the library keeps no count of its own.
     *
     * **Default `false` on every profile — the lean choice.** Closed out, the graph has no slot
     * for the hook, the subscribe and teardown paths carry no load or branch for it, and a
     * `graph_hooks_t::session_admission` handed to `graph_t::set_hooks` is not stored (a debug
     * build asserts on it, because a budget that silently does not apply is worse than none).
     *
     * **Who sets it.** An embedder that bounds what one session may subscribe to. The core test
     * build sets it in its preset fragment `core/tests/instrumented/libtracer/config_override.hpp`.
     * Override fragment: `static constexpr bool kSessionAdmission = true;`
     */
    static constexpr bool kSessionAdmission = false;

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

    /**
     * @brief The most learned `id ↔ path` bindings one CAN link holds at once (#2203).
     *
     * A receiver keeps a learned binding until a later advertise from the same node overlaps
     * its endpoint run (#909), so one sending node can hold up to its whole data-endpoint
     * window (`%net::kCanMaxGroupSlices`, 4095 slots) and every node it hears adds its own.
     * This bounds the sum per link. An advertise that would grow the table past it binds
     * nothing and is counted on `%net::can_transport_t::dropped_binding_cap()`; no binding is
     * ever evicted to make room. A re-issue that retires overlapping bindings first, or
     * rewrites its own slot in place, is never refused by it.
     *
     * Sizing: one binding is about 82 B on a 64-bit host, so the default of eight senders'
     * whole windows holds at most about 2.7 MB. A narrow target that hears fewer nodes, or
     * whose senders never lap their window, sets it to what it can afford; a counted
     * refusal is the sign it is too small. Override fragment:
     * `static constexpr std::size_t kCanLearnedBindings = 512;`
     */
    static constexpr std::size_t kCanLearnedBindings = 8 * 4095;
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

// The rename tripwire (#1703), permanent (#2247). A fragment written before the rename binds
// `reader_guard_t`, which nothing reads any more, so the build would silently keep the host
// `mutex_guard_t` — and on a dual-core chip with a lock-free bool no other assertion catches that.
// Refuse it.
static_assert(!defines_reader_guard_member<config_t>,
              "config_t defines reader_guard_t, which libtracer no longer reads: the member was "
              "renamed to guard_t (#1703). In your libtracer/config_override.hpp, rename "
              "`using reader_guard_t = ...;` to `using guard_t = ...;`.");

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
/** @brief @ref default_config_t::kInlineFanout for this build. */
inline constexpr std::size_t kInlineFanout = config_t::kInlineFanout;
static_assert(kInlineFanout >= 1,
              "kInlineFanout is at least 1: the inline snapshot buffer is "
              "an array of that many edge views");
/** @brief @ref default_config_t::kEdgePinSlots for this build. */
inline constexpr std::size_t kEdgePinSlots = config_t::kEdgePinSlots;
/** @brief @ref default_config_t::kDefaultShareHint for this build. */
inline constexpr share_hint_t kDefaultShareHint = config_t::kDefaultShareHint;
/**
 * @brief @p h as this build reads it: `DEFAULT` is @ref kDefaultShareHint, a declared `SHARE`
 *        or `COPY` is itself. The identity when @ref kDefaultShareHint is `DEFAULT`.
 */
[[nodiscard]] constexpr share_hint_t effective_share_hint(share_hint_t h) noexcept {
    return h == share_hint_t::DEFAULT ? kDefaultShareHint : h;
}
/** @brief @ref default_config_t::kDeferredReleaseSlots for this build. */
inline constexpr std::size_t kDeferredReleaseSlots = config_t::kDeferredReleaseSlots;
/**
 * @brief The participant count config @p C asks for: its own `kQsbrParticipants` when it
 *        declares one, else its `kEdgePinSlots` (#2111).
 *
 * `default_config_t::kQsbrParticipants` is initialized from `default_config_t::kEdgePinSlots`,
 * and a derived fragment that only re-declares `kEdgePinSlots` inherits that initializer's
 * value, not its derivation. A config that does not declare the knob names the base's very
 * object, so comparing the two members' addresses tells "inherited" from "declared".
 */
template <class C>
inline constexpr std::size_t qsbr_participants_v =
    &C::kQsbrParticipants == &default_config_t::kQsbrParticipants ? C::kEdgePinSlots
                                                                  : C::kQsbrParticipants;
/** @brief @ref default_config_t::kQsbrParticipants for this build; see `%qsbr_participants_v`. */
inline constexpr std::size_t kQsbrParticipants = qsbr_participants_v<config_t>;
/** @brief @ref default_config_t::kQsbrAsymmetricFence for this build. */
inline constexpr bool kQsbrAsymmetricFence = config_t::kQsbrAsymmetricFence;
/** @brief @ref default_config_t::kSingleCore for this build. */
inline constexpr bool kSingleCore = config_t::kSingleCore;
/** @brief @ref default_config_t::acl_policy_t for this build. */
using acl_policy_t = config_t::acl_policy_t;
/** @brief @ref default_config_t::kInstrumentCounters for this build. */
inline constexpr bool kInstrumentCounters = config_t::kInstrumentCounters;
/** @brief @ref default_config_t::kFaultInjection for this build. */
inline constexpr bool kFaultInjection = config_t::kFaultInjection;
/** @brief @ref default_config_t::kCreationHooks for this build. */
inline constexpr bool kCreationHooks = config_t::kCreationHooks;
/** @brief @ref default_config_t::kSessionAdmission for this build. */
inline constexpr bool kSessionAdmission = config_t::kSessionAdmission;
/** @brief @ref default_config_t::kForceGuardedRmw for this build. */
inline constexpr bool kForceGuardedRmw = config_t::kForceGuardedRmw;
/** @brief @ref default_config_t::kUdpStackScratchBytes for this build. */
inline constexpr std::size_t kUdpStackScratchBytes = config_t::kUdpStackScratchBytes;

namespace detail_guard {

/**
 * @brief The guard @p C binds, with the inherited host guard re-sized from @p C's own knobs.
 *
 * `default_config_t::guard_t` is spelled at the defaults (`mutex_guard_t`, 64 x 64), and a
 * member of a base cannot read a knob the derived fragment overrides. So a fragment that keeps
 * that binding but changes `kCacheLineBytes` or `kGuardStripes` gets the guard sized from ITS
 * values here (#1716). A fragment that names a guard itself gets exactly that guard.
 */
template <class C>
struct sized_guard {
    using type = typename C::guard_t; /**< @brief A guard the fragment bound itself. */
};
/** @brief The inherited default: the host guard at @p C's line size and stripe count. */
template <class C>
    requires std::is_same_v<typename C::guard_t, default_config_t::guard_t>
struct sized_guard<C> {
    /** @brief `basic_mutex_guard_t` at @p C's `kCacheLineBytes` and `kGuardStripes`. */
    using type = ::tr::basic_mutex_guard_t<C::kCacheLineBytes, C::kGuardStripes>;
};

}  // namespace detail_guard

/**
 * @brief The critical-section guard for this build: @ref default_config_t::guard_t, with the
 *        inherited host guard sized by `kCacheLineBytes` and `kGuardStripes` (#1716).
 */
using guard_t = detail_guard::sized_guard<config_t>::type;
/** @brief @ref default_config_t::kGuardStripes for this build. */
inline constexpr std::size_t kGuardStripes = config_t::kGuardStripes;
/** @brief @ref default_config_t::lkv_slot_t for this build. */
using lkv_slot_t = config_t::lkv_slot_t;
/** @brief @ref default_config_t::reclaim_policy_t for this build. */
using reclaim_policy_t = config_t::reclaim_policy_t;
static_assert(!std::is_same_v<reclaim_policy_t, reclaim_local_t>,
              "reclaim_local_t is withdrawn (#2127, ADR-0080 Amendment 1). Delete the "
              "`reclaim_policy_t` line to take the default, "
              "reclaim_qsbr_t, or bind reclaim_strict_t; see core/CHANGELOG.md, \"reclaim_local "
              "is withdrawn\", for the migration note.");

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

/** @brief @ref tr::graph::default_config_t::kSlabPool for this build (#1777). */
inline constexpr bool kSlabPool = tr::graph::config_t::kSlabPool;
/** @brief @ref tr::graph::default_config_t::kSlabBytes for this build (#1777). */
inline constexpr std::size_t kSlabBytes = tr::graph::config_t::kSlabBytes;
/** @brief @ref tr::graph::default_config_t::kSlabClassCap for this build (#1777). */
inline constexpr std::size_t kSlabClassCap = tr::graph::config_t::kSlabClassCap;
/** @brief @ref tr::graph::default_config_t::kArenaBytes for this build (#1783). */
inline constexpr std::size_t kArenaBytes = tr::graph::config_t::kArenaBytes;
/**
 * @brief Whether this build's default root is the platform heap (#2090): `kSlabPool` is
 *        `false` and `kArenaBytes` is 0, so no static arena is compiled and
 *        `tr::mem::default_root()` is `tr::mem::heap_source()`.
 */
inline constexpr bool kHeapRoot = !kSlabPool && kArenaBytes == 0;

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

/**
 * @brief Frames a receive context consumes back to back before it waits for its core to
 *        idle; `0` = no frame budget.
 *
 * The transport plane's spelling of @ref tr::graph::default_config_t::kRxDrainFrames, which
 * carries the rationale. Its one consumer is the ESP-IDF `httpd_ws_link_t`.
 */
inline constexpr std::size_t kRxDrainFrames = tr::graph::config_t::kRxDrainFrames;

/**
 * @brief Payload bytes a receive context consumes back to back before it waits for its core
 *        to idle; `0` = no byte budget.
 *
 * The transport plane's spelling of @ref tr::graph::default_config_t::kRxDrainBytes, which
 * carries the rationale. Its one consumer is the ESP-IDF `httpd_ws_link_t`.
 */
inline constexpr std::size_t kRxDrainBytes = tr::graph::config_t::kRxDrainBytes;

}  // namespace tr::net
