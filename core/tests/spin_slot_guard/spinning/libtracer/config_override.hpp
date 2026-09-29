/**
 * @file
 * @brief Test fixture — the `spin_slot_guard` SPINNING arm: a slot policy that declares `may_spin =
 * true` on a target that refuses spin-waiting.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This must NOT compile, and must be refused by the spin assertion in `vertex.hpp`.
 */
#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace tr::view {
class rope_t;
}  // namespace tr::view

namespace tr::graph {

/** @brief Test-only LKV slot policy: a complete implementation of the contract whose
 *         spin declaration is what the arm varies. */
class probe_slot_t {
   public:
    using value_ptr_t = std::shared_ptr<const view::rope_t>; /**< @brief The owning handle. */
    static constexpr bool may_spin = true;                   /**< @brief It spins, and says so. */

    /** @brief Publish. */
    [[nodiscard]] bool store(value_ptr_t sp, std::memory_order = std::memory_order_seq_cst) {
        v_ = std::move(sp);
        return true;
    }
    /** @brief Drop the published value. */
    void clear(std::memory_order = std::memory_order_seq_cst) { v_.reset(); }
    /** @brief Read the published value. */
    [[nodiscard]] value_ptr_t load() const { return v_; }

   private:
    value_ptr_t v_{};
};

/** @brief The defaults with spin-waiting refused and the probe policy bound. */
struct spin_slot_spinning_config_t : default_config_t {
    static constexpr bool kSpinWaitSafe = false;
    using lkv_slot_t = probe_slot_t;
};

using config_t = spin_slot_spinning_config_t;

}  // namespace tr::graph
