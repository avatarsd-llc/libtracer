/**
 * @file
 * @brief Test fixture — the `spin_slot_guard` UNMARKED arm: a slot policy that never declares
 * `may_spin` at all.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This must NOT compile, and must be refused by the declaration assertion in `vertex.hpp`: a
 * policy cannot slip past
 * the spin assertion by forgetting the declaration it reads.
 */
#pragma once

#include <atomic>
#include <utility>

#include "libtracer/value.hpp"

namespace tr::graph {

/** @brief Test-only LKV slot policy: a complete implementation of the contract whose
 *         spin declaration is what the arm varies. */
class probe_slot_t {
   public:
    probe_slot_t() = default;
    probe_slot_t(const probe_slot_t&) = delete;
    probe_slot_t& operator=(const probe_slot_t&) = delete;
    ~probe_slot_t() { clear(); }

    /** @brief Publish: the slot adopts @p v's reference and drops the one it held. */
    [[nodiscard]] bool store(value_t* v, std::memory_order = std::memory_order_seq_cst) {
        value_t* const old = std::exchange(v_, v);
        if (old != nullptr) value_t::release(old);
        return true;
    }
    /** @brief Drop the published value. */
    void clear(std::memory_order = std::memory_order_seq_cst) { (void)store(nullptr); }
    /** @brief Read the published value: one more reference to it. */
    [[nodiscard]] value_ref_t load() const { return value_ref_t::share(v_); }

   private:
    value_t* v_ = nullptr; /**< @brief The one published value, or null. */
};

/** @brief The defaults with spin-waiting refused and the probe policy bound. */
struct spin_slot_unmarked_config_t : default_config_t {
    static constexpr bool kSpinWaitSafe = false;
    using lkv_slot_t = probe_slot_t;
};

using config_t = spin_slot_unmarked_config_t;

}  // namespace tr::graph
