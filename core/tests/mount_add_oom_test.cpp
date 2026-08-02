/**
 * @file
 * @brief `add_child` under a REFUSING allocator: a registry that cannot grow registers
 *        nothing, says so, and leaves no ghost child behind (#523).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section why Why this is its own executable
 *
 * The only allocation on `add_child`'s registry path that can fail SOFTLY is the registry
 * chunk: `child_registry_t::append` asks for it with `new (std::nothrow) chunk_t()` and reads
 * a null back rather than throwing. That is the seam, and the only way to drive it from a test
 * is to replace the global NOTHROW `operator new` — a whole-program decision, so it lives in
 * its own binary where nothing else is affected. The THROWING form is left alone, so every
 * `std::string` / `std::vector` in the test (and in `add_child` itself, which encodes the
 * mount run before it appends) allocates normally; only the seam is armed.
 *
 * @section what What it pins
 *
 * `add_child` used to call `registry_.add` for its side effect and return `true` regardless.
 * With the chunk refused, `add` soft-failed and registered NOTHING, and `add_child` went on to
 * `set_receiver` on the link and report success — a GHOST: audible on its transport, resolvable
 * by no `dst` (`by_name` misses it), and removable by no `remove_child`. That is the same
 * "healthy-looking child every forward misses" shape #523 was filed about, one step further
 * out, and it is exactly what the new `bool` return exists to stop.
 *
 * @section ablation What goes RED if the guard is ablated
 *
 * Restore `registry_.add(name, link);` (ignoring the result) in `fwd_router_t::add_child` and
 * `refused_registration_reports_failure` goes red on its FIRST assertion — the call reports
 * success. Restore `void add(...)` with its `if (slot == nullptr) return;` and it fails to
 * compile, which is the same guard one level down.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string_view>

#include "libtracer/tracer.hpp"

namespace {

int g_failures = 0;

/** @brief Record one assertion. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/**
 * @brief A link that counts what it is asked to send and can say whether it was WIRED.
 *
 * The wiring is the ghost's other half, and it is the half a `live_size()` check cannot see:
 * `add_child` installs the inbound receiver on the link AFTER the registry call, so a router
 * that ignored a failed registration would leave a link delivering frames into a router that
 * can route none of them. `rx_` is the transport's own protected delivery slot, so asking it
 * directly is asking the seam rather than a proxy for it.
 */
struct counting_link_t : tr::net::transport_t {
    std::size_t sent = 0; /**< @brief Frames handed to this endpoint. */
    void send(std::span<const std::byte>) override { ++sent; }
    /** @brief True ⇔ some inbound sink has been installed on this link. */
    [[nodiscard]] bool wired() const noexcept { return rx_.has_any(); }
};

}  // namespace

/**
 * @brief True while the global nothrow `operator new` must refuse.
 *
 * Namespace-scope and non-static because the replacement below is a global function and this
 * is the only state it may read. Armed for the width of ONE `add_child` call.
 */
bool g_refuse_nothrow_new = false;

/**
 * @brief The refusing seam: a null back, exactly as an exhausted heap gives.
 *
 * Disarmed, it forwards to the THROWING global form rather than to `std::malloc`, so every
 * block handed out here is still freed by the matching `::operator delete` the library's own
 * `probe_bytes` pairs it with. Replacing the pairing too would only invite a mismatch.
 */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_refuse_nothrow_new) return nullptr;
    try {
        return ::operator new(n);
    } catch (...) {
        return nullptr;
    }
}

namespace {

/** @brief A refused chunk means: no registration, no receiver, no ghost. */
void test_refused_registration_reports_failure() {
    std::printf("add_child under a refusing allocator\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    counting_link_t link;

    g_refuse_nothrow_new = true;  // the FIRST chunk the registry asks for is refused
    const bool added = router.add_child("net/ws/a", link);
    g_refuse_nothrow_new = false;

    check(!added, "add_child REPORTS the failure rather than returning true");
    check(router.registry().live_size() == 0, "nothing is registered");
    check(router.registry().by_name("net/ws/a") == nullptr, "and no dst can resolve the name");

    // The receiver seam is the ghost's other half: a wired link keeps delivering into a
    // router that holds no entry for it. Nothing was wired at all.
    check(!link.wired(), "and NO inbound receiver was installed on the link");
}

/** @brief The same call with the allocator open still registers — the refusal is the ONLY
 *         difference, so the red above cannot be the test refusing everything. */
void test_open_allocator_still_registers() {
    std::printf("add_child with the allocator open\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    counting_link_t link;
    const bool added = router.add_child("net/ws/a", link);
    check(added, "add_child succeeds");
    check(link.wired(), "the inbound receiver IS installed — so the check above is not vacuous");
    check(router.registry().live_size() == 1, "the child is registered");
    check(router.registry().by_name("net/ws/a") == &link, "and the name resolves to the link");
}

}  // namespace

int main() {
    test_refused_registration_reports_failure();
    test_open_allocator_still_registers();
    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
