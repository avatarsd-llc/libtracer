/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * rmw_counter — a counter whose bump is one hardware RMW where the target has one, and a
 * guarded load + store where it has none (#1621, RFC-0028 D6).
 */
#pragma once

#include <atomic>
#include <type_traits>

#include "libtracer/reader_guard.hpp"

/**
 * @file
 * @brief `rmw_counter_t`: a wrapping counter that costs one atomic add on a target with atomic
 *        RMW, and one section of the build's critical-section guard on a target without.
 */

namespace tr::graph {

/**
 * @brief A wrapping counter, bumped by many writers and read without a lock, whose bump is
 *        chosen at compile time from what the target's hardware can do.
 *
 * - **Native** (`std::atomic<T>::is_always_lock_free`): the bump is one `fetch_add` — `lock
 *   xadd` on x86-64, `amoadd.w` on rv32imac (ESP32-C6), `ldrex`/`strex` on Cortex-M3/M4/M7
 *   (both cores of an STM32H7), `s32c1i` on Xtensa. No call, no masked interrupt.
 * - **Guarded** (no atomic RMW: rv32imc such as the ESP32-C3, Cortex-M0/M0+): the bump is a
 *   load and a store inside one section of @p G, the build's ONE critical-section type
 *   (`config_t::reader_guard_t`, RFC-0028 §5.5) — an interrupt mask on a single-core chip, and
 *   on a dual-core one the cross-core lock `G` already is for the LKV slot. Plain aligned
 *   loads and stores stay single instructions on these targets (rv32imc compiles them to
 *   `fence; lw; fence`), so only the bump pays, and it pays the same section libatomic would
 *   open for it, without the call.
 *
 * A guarded bump that skipped the guard would be unsound with two writers: one that loaded
 * `n` and was preempted can store `n + 1` after a later writer's `n + 2`, rewinding the counter
 * to a value a reader already snapshotted, and that reader then misses the later change. The
 * guard serializes the writers; the reader needs no guard, because every store is a whole
 * aligned word.
 *
 * Both bindings give the bump `seq_cst` ordering, so a caller's Dekker pair (bump, then read a
 * flag; set the flag, then read the counter) holds on either. The counter only ever moves by
 * one and wraps at `T`'s width, so it is for EQUALITY tests (`now != then`), never `<`.
 *
 * @tparam T       An unsigned integer, at most a machine word wide.
 * @tparam G       The build's critical-section guard (`tr::graph::reader_guard`), taken only
 *                 by the guarded binding.
 * @tparam kNative Which binding. Defaults to what the target supports; a test names it to
 *                 drive the guarded binding on a host that has atomic RMW.
 */
template <class T, class G, bool kNative = std::atomic<T>::is_always_lock_free>
class rmw_counter_t {
    static_assert(std::is_unsigned_v<T>, "the counter wraps, so it must be unsigned");

   public:
    /** @brief Whether the bump is one hardware RMW (`true`) or a guarded load + store. */
    static constexpr bool is_native = kNative;

    /** @brief Move the counter on by one, `seq_cst`, wrapping at `T`'s width. */
    void bump() noexcept {
        if constexpr (kNative) {
            value_.fetch_add(1, std::memory_order_seq_cst);
        } else {
            static_assert(reader_guard<G>, "the guarded bump needs the build's reader guard");
            const guard_scope_t<G> section(this);
            value_.store(static_cast<T>(value_.load(std::memory_order_relaxed) + 1u),
                         std::memory_order_seq_cst);
        }
    }

    /** @brief The current count, `seq_cst`. Lock-free on both bindings. */
    [[nodiscard]] T load() const noexcept { return value_.load(std::memory_order_seq_cst); }

    /**
     * @brief Set the count outright. For a test that must reach the wrap without 2^32 bumps,
     *        and for nothing else: a store races every bump.
     */
    void preset(T value) noexcept { value_.store(value, std::memory_order_seq_cst); }

   private:
    std::atomic<T> value_{0}; /**< @brief The count; stores and loads are whole words. */
};

}  // namespace tr::graph
