/**
 * @file
 * @brief The LKV slot policy (ADR-0069 §1): how a vertex publishes and reads its
 *        last-known value.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `vertex_t` owns exactly one slot and touches it through three operations — publish,
 * clear, read. Naming that surface is what lets a target choose its reclamation strategy
 * at build time (ADR-0069) instead of every target paying for one compromise: a
 * single-core sensor node is write-dominated and wants the cheapest publish, while a
 * many-core host's cost is the read side, which today INVERTS under concurrent readers
 * (measured: 15.4 M `graph_t::read`/s at one reader falling to 1.8 at twenty-four —
 * `bench/bench_lkv_slot.cpp`, `lkvgraph_hot1-fan0-read`).
 *
 * ## The policy contract
 *
 * A slot type must provide, for `value_ptr_t = std::shared_ptr<const view::rope_t>`:
 *
 *   - `store(value_ptr_t)` — publish, sequentially consistent. `vertex_t::store` relies on
 *     this sharing one total order with the `write_seq_` bump and the waiter count, which
 *     is what makes the waiterless publish (#555) unable to lose a wakeup.
 *   - `store(value_ptr_t, std::memory_order)` — publish under a caller-chosen order. Only
 *     `revert_to_placeholder` uses it, to clear with `release`.
 *   - `load() const` — read the published value.
 *
 * ## The constraint any future slot must satisfy
 *
 * `load()` returns an **owning** handle, and that is not negotiable: `graph_t`'s composed
 * branch read (`read_subtree_folded`) stashes one LKV per node into a vector that outlives
 * the map lock and spans three passes, so **N values are held simultaneously**. A scheme
 * that can protect only one value per reader at a time — hazard pointers, as classically
 * stated — therefore cannot simply hand back a pinned pointer; it must promote the pin to
 * a counted reference before releasing it, and that promotion is a read-modify-write on
 * the one cache line every reader shares. Measured, that promotion is the difference
 * between a 1,806x and a 20.8x read win at twenty-four readers, which is why ADR-0069
 * carries the smaller number (see #642, and the erratum in #643).
 */
#pragma once

#include <atomic>
#include <memory>

#include "libtracer/rope.hpp"

namespace tr::graph {

/**
 * @brief The slot libtracer ships today: `std::atomic<std::shared_ptr<const rope_t>>`.
 *
 * Reclamation is the shared_ptr refcount, so there is no scheme to implement and no
 * registry to size — the reason this is the checked-in default, and the reason a raw `-I`
 * consumer and the stock ESP-IDF component keep building exactly what they built before
 * the slot became a policy.
 *
 * **Lock-free BY CONTRACT, and spin-locked in practice.**
 * `std::atomic<std::shared_ptr<T>>::is_lock_free()` returns 0 on libstdc++, so both load
 * and store take its internal pointer-lock bit (`lock cmpxchg` to acquire, an `xchg` to
 * release). Measured, that is ~77 of the ~316 cycles of an in-process write and the
 * largest single term left on the path — 88% of `store`'s samples land on those three
 * instructions. Do not read "lock-free" here as "no serializing operation"; ADR-0064 §2
 * records why, and ADR-0069 records what replaces it on a host.
 */
class sp_atomic_slot_t {
   public:
    /** @brief The handle a publish takes and a read returns — owning, by the contract above. */
    using value_ptr_t = std::shared_ptr<const view::rope_t>;

    /**
     * @brief Publish. Sequentially consistent unless the caller says otherwise — the default
     *        is what orders the publish with `write_seq_` and the waiter count, which is what
     *        makes the waiterless publish (#555) unable to lose a wakeup.
     *
     * One function with a defaulted order, mirroring `std::atomic<T>::store`, rather than two
     * overloads: it keeps this policy's call shape identical to the member it replaced.
     */
    void store(value_ptr_t sp, std::memory_order order = std::memory_order_seq_cst) {
        v_.store(std::move(sp), order);
    }

    /**
     * @brief Read the published value.
     *
     * A mid-read reader holds its own reference, so a concurrent publish or clear cannot
     * free the value under it — that is the whole of this policy's reclamation.
     */
    [[nodiscard]] value_ptr_t load() const { return v_.load(); }

   private:
    std::atomic<value_ptr_t> v_{};
};

}  // namespace tr::graph
