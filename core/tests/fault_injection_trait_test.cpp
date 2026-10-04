/**
 * @file
 * @brief #1719 — the test-only fault-injection hooks are `config_t::kFaultInjection`, lean by
 *        default, and `rmw_counter_t`'s preset is reachable only through a test door.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What this pins:
 *
 *  - **The shipped default carries no hook.** `default_config_t` binds `kFaultInjection =
 *    false`, and a fragment that derives from it to set another knob inherits `false`.
 *  - **The gate follows the binding.** In THIS build, an armed `probe_fail_hook` refuses a
 *    draw from the process-default heap source exactly when the build opted in; on a build
 *    that did not, the same arming changes nothing, because the check is compiled out rather
 *    than skipped at run time. Both arms are exercised by the same executable: the core test
 *    build opts in through its preset (`core/tests/instrumented`), a CI leg binding its own
 *    fragment may not.
 *  - **`rmw_counter_t::preset` is not public.** A store that races every bump is a test tool;
 *    only `tr::rmw_counter_test_door_t`, which the library declares and never defines, reaches
 *    it.
 *
 * The symbol half of the claim (no hook variable in a default archive) is checked on the real
 * default library by the `install-consume` CI job, which builds it with no fragment at all.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "libtracer/config.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/mem_source.hpp"
#include "test_support.hpp"

namespace tr {

/** @brief The test-only door `guard.hpp` declares (#1719): presets an `rmw_counter_t`. */
struct rmw_counter_test_door_t {
    /** @brief Store @p value into @p c outright (`seq_cst`, like the bump). */
    template <class T, class G, bool kNative>
    static void preset(rmw_counter_t<T, G, kNative>& c, T value) noexcept {
        c.preset(value);
    }
};

}  // namespace tr

namespace {

using tr::graph::default_config_t;
using tr::testing::check;

/** @brief A fragment in the ADR-0068 idiom that changes an unrelated knob and nothing else. */
struct unrelated_knob_config_t : default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;
};

/** @brief A fragment that opts in, as the core test preset does. */
struct opted_in_config_t : default_config_t {
    static constexpr bool kFaultInjection = true;
};

/** @brief Whether @p C exposes a callable `preset` to code outside the class. */
template <class C>
concept has_public_preset = requires(C& c) { c.preset(0u); };

/** @brief A counter on the guarded binding, named on a host that has atomic RMW. */
using guarded_counter_t = tr::rmw_counter_t<std::uint32_t, tr::no_guard_t, false>;
/** @brief A counter on the native binding. */
using native_counter_t = tr::rmw_counter_t<std::uint32_t, tr::no_guard_t, true>;

static_assert(!has_public_preset<guarded_counter_t>,
              "rmw_counter_t::preset is not public API (#1719): only the test door reaches it");
static_assert(!has_public_preset<native_counter_t>,
              "rmw_counter_t::preset is not public API on the native binding either");

/** @brief A hook that refuses every request. */
bool refuse_all(std::size_t) noexcept { return false; }

/** @brief The stock default carries no hook; inheriting is not opting in; opting in is one line. */
void test_default_is_lean() {
    check(!default_config_t::kFaultInjection,
          "default_config_t::kFaultInjection is false — the hooks are test-build only (#1719)");
    check(!unrelated_knob_config_t::kFaultInjection,
          "a fragment that only sets an unrelated knob inherits kFaultInjection = false");
    check(opted_in_config_t::kFaultInjection, "binding kFaultInjection = true opts in");
}

/** @brief An armed `probe_fail_hook` refuses a default-source draw iff this build opted in. */
void test_gate_follows_the_binding() {
    constexpr bool kOn = tr::graph::kFaultInjection;
    std::printf("this build: kFaultInjection = %s\n", kOn ? "true" : "false");

    tr::detail::probe_fail_hook = &refuse_all;
    const bool gate_ok = tr::detail::probe_hook_ok(64);
    void* const p = tr::mem::heap_source().try_alloc(64, alignof(std::max_align_t));
    tr::detail::probe_fail_hook = nullptr;

    if (p != nullptr) tr::mem::heap_source().release(p, 64, alignof(std::max_align_t));
    check(gate_ok == !kOn, "probe_hook_ok honours an armed hook exactly when kFaultInjection");
    check((p == nullptr) == kOn,
          "the default heap source refuses under an armed hook exactly when kFaultInjection");

    // Disarmed, the gate always admits, on either binding.
    check(tr::detail::probe_hook_ok(64), "a disarmed seam admits every request");
}

/** @brief The door reaches the wrap on both bindings without 2^32 bumps. */
void test_door_presets_both_bindings() {
    guarded_counter_t g;
    tr::rmw_counter_test_door_t::preset(g, 0xFFFFFFFFu);
    g.bump();
    check(g.load() == 0u, "the door presets the guarded binding, and the bump wraps to 0");

    native_counter_t n;
    tr::rmw_counter_test_door_t::preset(n, 0xFFFFFFFEu);
    n.bump();
    check(n.load() == 0xFFFFFFFFu, "the door presets the native binding");
}

}  // namespace

/** @brief Runs the cases. */
int main() {
    test_default_is_lean();
    test_gate_follows_the_binding();
    test_door_presets_both_bindings();
    return tr::testing::summary("fault_injection_trait");
}
