/**
 * @file
 * @brief The EDGE-PIN domain (#635): a bounded, per-participant announcement registry that
 *        lets a publisher copy a vertex's published edge array out without taking any lock,
 *        and lets the control plane free a displaced array without waiting for anybody.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This is the announce/scan PROTOCOL shape of a hazard pointer applied to exactly ONE site —
 * `vertex_t::snapshot_edges` — and nothing else. It is deliberately NOT the generalized,
 * graph-owned, type-erased reclamation domain ADR-0072 proposed and PR #750 got rejected for:
 * there is no registered tenant list, nothing is added to any READ path, no embedder-owned
 * destructor can run inside it (a published edge array owns only library types), and a
 * participant never waits for another participant. What it borrows is the two-store dance and
 * the scan, which are the parts that were never in dispute.
 *
 * Why a pin is admissible here when ADR-0072's supersession said fan-out could not hold one:
 * `snapshot_edges` already COPIES OUT (`edge_view_t` holds refcount shares) and `graph_t::fan_out`
 * dispatches OUTSIDE the vertex lock. The window a pin has to cover is therefore the copy loop
 * alone — bounded, no I/O, and provably not re-entrant, because the pin is released before the
 * first `dispatch_edge` call. A subscriber callback that re-enters the graph (a nested wide
 * fan-out, a bubbling ancestor delivery) therefore always finds this thread's cell EMPTY, which
 * `pin_t`'s debug assertion states as a checked invariant rather than a comment.
 *
 * **On one hart** (@ref tr::graph::kSingleCore) the announce, the validating load, the clear and
 * the scan are relaxed accesses ordered by compiler barriers (#2126's COMPILER flavour, applied
 * here by #2122). The argument is `qsbr.hpp`'s, section "The single-hart flavour", unchanged:
 * with one hart no store buffering is observable across tasks, and a context switch is a full
 * barrier, so the Dekker pairing below (announce, then validate; exchange, then scan) holds in
 * hart order once the compiler keeps each pair in program order. Everywhere else the protocol
 * is the `seq_cst` one, compiled exactly as before.
 *
 * The announcement is type-erased over `const void*` on purpose: the pinned type
 * (`tr::graph::edge_pub_t`) is declared inside `%vertex.hpp`, which includes THIS header. Erasing
 * the type at the cell keeps the dependency pointing one way and costs a cast.
 */
#ifndef LIBTRACER_EDGE_PIN_HPP
#define LIBTRACER_EDGE_PIN_HPP

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "libtracer/config.hpp"
#include "libtracer/qsbr.hpp"

namespace tr::graph::detail_ep {

/**
 * @brief The domain's padding width: @ref tr::graph::kCacheLineBytes, floored at the
 *        announcement's natural alignment so every value of the knob stays well-formed.
 *
 * A single-core target sets the knob to 0 and the cell collapses to its payload — there is no
 * second core for a scan to false-share against. See `kCacheLineBytes` for why `alignas` may
 * not simply be handed the number.
 */
inline constexpr std::size_t kCellAlign = kCacheLineBytes > alignof(std::atomic<const void*>)
                                              ? kCacheLineBytes
                                              : alignof(std::atomic<const void*>);

/**
 * @brief One participant's announcement, cache-line isolated so a scan does not false-share
 *        against it — and, more to the point, so the announcing STORE lands on a line no other
 *        reader of the same vertex touches.
 *
 * That isolation is the whole performance argument (#635): the mechanism this replaces is a
 * shared stripe mutex (two lock-prefixed RMWs on a line every publisher of the stripe shares),
 * and the mechanism it deliberately is NOT is a refcounted published array (an RMW on a line
 * every reader of the VERTEX shares). A `seq_cst` store to this thread's own cell is neither.
 */
struct alignas(kCellAlign) cell_t {
    std::atomic<const void*> pinned{nullptr}; /**< @brief The array this thread is copying. */
    std::atomic<bool> claimed{false};         /**< @brief Whether a live thread owns this index. */
};

static_assert(alignof(cell_t) == kCellAlign,
              "the cell's alignas was silently dropped — see kCellAlign's derivation");

/**
 * @brief The order of the pin protocol's accesses: `seq_cst` on more than one core, `relaxed`
 *        on one hart, where @ref hart_barrier keeps their program order (see the file comment).
 */
inline constexpr std::memory_order kPinOrder =
    kSingleCore ? std::memory_order_relaxed : std::memory_order_seq_cst;

/** @brief The clear's order: `release` on more than one core, `relaxed` on one hart. */
inline constexpr std::memory_order kClearOrder =
    kSingleCore ? std::memory_order_relaxed : std::memory_order_release;

/** @brief A compiler barrier on one hart; nothing elsewhere, where the orders above suffice. */
inline void hart_barrier() noexcept {
    if constexpr (kSingleCore) std::atomic_signal_fence(std::memory_order_seq_cst);
}

/** @brief "This thread has not claimed an index" — the fallback reader's signal. */
inline constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

/**
 * @brief The domain's storage: `kEdgePinSlots` claimable announcements.
 *
 * `constinit` and trivially destructible on purpose. It lands in `.bss` with no guard variable
 * and is never destroyed, so a thread's exit release can always reach it — the ordering hazard a
 * `std::vector` or a `std::mutex` in here would create. It owns NOTHING (an announcement is a
 * borrowed pointer), so there is no exit sweep to register either.
 */
struct registry_t {
    std::array<cell_t, kEdgePinSlots> cells{}; /**< @brief The announcements. */
};

/** @brief The one domain. */
[[nodiscard]] inline registry_t& registry() {
    static constinit registry_t reg{};
    return reg;
}

/**
 * @brief Gives this thread's edge-pin cell back when the thread ends. Constructed only by
 *        @ref claim_pin, so a pin never touches a guarded object.
 *
 * The announcement is cleared first, so a scan that races this release sees an empty cell. The
 * record's pin is then left at "none", so a pin a later `thread_local` destructor takes uses
 * the stripe-mutex fallback instead of a cell another thread may own by then.
 */
struct pin_release_t {
    pin_release_t() = default;
    pin_release_t(const pin_release_t&) = delete;
    pin_release_t& operator=(const pin_release_t&) = delete;
    /** @brief Give the cell back; see the class. */
    ~pin_release_t() {
        detail_qsbr::thread_record_t& t = detail_qsbr::rec();
        if (t.pin != 0 && t.pin != detail_qsbr::kNoCell) {
            cell_t& c = registry().cells[t.pin - 1];
            c.pinned.store(nullptr, std::memory_order_seq_cst);
            c.claimed.store(false, std::memory_order_release);
        }
        t.pin = detail_qsbr::kNoCell;
    }
};

/**
 * @brief Claim an edge-pin cell for this thread, once, into its record: the table walk, out of
 *        line.
 * @return The claimed index, or @ref kNoIndex when every index is already taken (the caller
 *         then takes the stripe-mutex fallback — correctness is unaffected). A full table is
 *         not re-walked on every publish: the fallback reader already pays a mutex, and an
 *         O(N) CAS sweep on top would turn "undersized knob" into a second, larger cost.
 */
[[gnu::noinline]] inline std::size_t claim_pin(detail_qsbr::thread_record_t& t) noexcept {
    registry_t& r = registry();
    for (std::size_t i = 0; i < kEdgePinSlots; ++i) {
        bool expected = false;
        if (r.cells[i].claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
            [[maybe_unused]] static thread_local pin_release_t release;
            t.pin = static_cast<std::uint32_t>(i + 1);
            return i;
        }
    }
    t.pin = detail_qsbr::kNoCell;
    return kNoIndex;
}

/** @brief This thread's edge-pin index, claimed on first use, or @ref kNoIndex. */
[[nodiscard]] inline std::size_t index() noexcept {
    detail_qsbr::thread_record_t& t = detail_qsbr::rec();
    const std::uint32_t c = t.pin;
    if (c == 0) [[unlikely]]
        return claim_pin(t);
    return c == detail_qsbr::kNoCell ? kNoIndex : c - 1;
}

/**
 * @brief One thread's edge pin, scoped to a copy-out and NOTHING else.
 *
 * Non-nestable BY CONSTRUCTION — one cell per thread — and the constructor asserts the cell is
 * empty, which is what turns "the pin is never held across `dispatch_edge`" from a claim in a
 * comment into a checked invariant. A re-entrant subscriber callback that reached a second
 * `snapshot_edges` while the outer pin was still held would fire this assertion in every debug
 * build and in the CI ASan/TSan legs.
 */
class pin_t {
   public:
    /** @brief Resolve this thread's cell (or none, when the domain is full). */
    pin_t() noexcept : idx_(index()) {
        assert((idx_ == kNoIndex ||
                registry().cells[idx_].pinned.load(std::memory_order_relaxed) == nullptr) &&
               "an edge pin was still held on re-entry — it must be released before dispatch");
    }
    pin_t(const pin_t&) = delete;
    pin_t& operator=(const pin_t&) = delete;
    /**
     * @brief Drop the announcement. The pin's scope ends before the caller dispatches, so the
     *        one clear is the destructor's: a second, explicit one was a second release store
     *        (a `fence rw,w` and a `sw` on rv32) per fan-out.
     */
    ~pin_t() {
        if (idx_ == kNoIndex) return;
        hart_barrier();  // the copy-out stays above the clear
        registry().cells[idx_].pinned.store(nullptr, kClearOrder);
    }

    /** @brief False ⇒ the domain is full; the caller takes the stripe-mutex fallback. */
    [[nodiscard]] bool valid() const noexcept { return idx_ != kNoIndex; }

    /**
     * @brief Announce and validate: publish @p src's current value into this thread's cell,
     *        then re-read @p src and retry until the two agree.
     *
     * The classic protocol. Both the announcing store and the validating load are `seq_cst`
     * so they share ONE total order with the mutator's `exchange` on @p src and its subsequent
     * scan of the cells: if the validating load still saw @p p published, the mutator had not
     * yet displaced it, so the mutator's scan is ordered after this store and observes it.
     * Writers are control-plane-rare, so the retry is not a loop anybody spins in. On one hart
     * both are relaxed, with a compiler barrier between them and after the validation, which
     * keeps the copy-out below it (see the file comment).
     *
     * @return The pinned value (possibly null — an edgeless vertex publishes nothing).
     */
    template <class T>
    [[nodiscard]] T* acquire(const std::atomic<T*>& src) noexcept {
        cell_t& c = registry().cells[idx_];
        T* p = src.load(kPinOrder);
        for (;;) {
            c.pinned.store(p, kPinOrder);
            hart_barrier();
            T* q = src.load(kPinOrder);
            hart_barrier();
            if (q == p) return p;
            p = q;
        }
    }

   private:
    std::size_t idx_;
};

/**
 * @brief Is @p p announced by any participant right now?
 *
 * The scan half, run by the MUTATOR on its own thread outside every lock. `seq_cst` loads for
 * the reason @ref pin_t::acquire gives (relaxed after a compiler barrier on one hart, which
 * keeps the caller's displacing `exchange` above the scan). O(`kEdgePinSlots`) and reached only
 * on a control-plane verb, so it is never on a delivery path.
 */
[[nodiscard]] inline bool is_pinned(const void* p) noexcept {
    registry_t& r = registry();
    hart_barrier();
    for (std::size_t i = 0; i < kEdgePinSlots; ++i)
        if (r.cells[i].pinned.load(kPinOrder) == p) return true;
    return false;
}

}  // namespace tr::graph::detail_ep

#endif  // LIBTRACER_EDGE_PIN_HPP
