/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The probe TU for the #1158 spin-pool guard. Compiled TWICE by cmake/spin_pool_guard.cmake,
 * against two renderings of libtracer/config.hpp: it must compile where kSpinWaitSafe is true
 * and must FAIL where it is false. Not an add_executable() target — the check drives the
 * compiler directly, because what is under test is whether a build is rejected.
 */
#include <array>
#include <cstddef>
#include <span>

#include "libtracer/mem_pool.hpp"

/**
 * @file
 * @brief Instantiates a `synchronized_pool_t` over a guard that declares it MAY SPIN — the
 *        shape the guard must reject where spin-waiting is unsafe.
 *
 * Since RFC-0028 slice 10 no shipped guard pure-spins (the host `mutex_guard_t` naps after a
 * bounded spin, the ESP-IDF `critical_guard_t` masks interrupts), so the probe brings its own:
 * the check is about the TRAIT the pool reads (`may_spin`), whoever declares it.
 */

namespace {

/** @brief A deliberately spinning guard: the `tr::guard` shape with `may_spin = true`. */
struct spinning_guard_t {
    static constexpr bool is_isr_safe = false;           /**< @brief Not an ISR section. */
    static constexpr bool is_nonblocking = true;         /**< @brief Spins, never sleeps. */
    static constexpr bool may_spin = true;               /**< @brief The trait under test. */
    static constexpr const char* name = "probe_spinner"; /**< @brief Diagnostic name. */
    /** @brief Spin in. */
    void lock() noexcept {}
    /** @brief Spin out. */
    void unlock() noexcept {}
    /** @brief The `tr::guard` lookup. */
    static spinning_guard_t& for_address(const void*) noexcept {
        static spinning_guard_t g;
        return g;
    }
};

/** @brief A slab the pool can carve; its size is irrelevant to the guard. */
std::array<std::byte, 256> g_slab{};

/** @brief The instantiation under test — declaring one is what trips the `static_assert`. */
tr::mem::synchronized_pool_t<spinning_guard_t> g_pool{std::span<std::byte>(g_slab), 64};

}  // namespace

/** @brief Keeps @ref g_pool odr-used so no toolchain can elide the instantiation. */
int main() { return g_pool.capacity() == 0 ? 1 : 0; }
