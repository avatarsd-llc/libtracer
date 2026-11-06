/**
 * @file
 * @brief `hop_mint` refuses a removed child on its own (#2560): by name, and through the
 *        retired receiver context it left behind.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `child_rx_churn_test` checks "a removed child mints no reference" end to end, but the #2302
 * ingress drop now stops a removed child's frames before `hop_mint` runs, so that test passes
 * whatever `hop_mint` does. Only a race between removal and ingress reaches the mint's own
 * tombstone handling. This file calls it directly through a test-only door and pins both
 * guards:
 *   - `ctx_by_name` skips a retired context, so a lookup by name finds nothing;
 *   - `hop_mint` re-checks `retired` on a context it was handed, so the context a removed
 *     child left behind mints nothing.
 *
 * The connection vertex stays registered throughout, so `vertex_slot_at` would still answer:
 * the refusal has to come from the context's tombstone.
 */

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace tr::net {
/** @brief Test-only door: the private reply-mint path, called directly. */
struct fwd_router_test_door_t {
    /** @brief The router's private receiver-context type. */
    using ctx_t = fwd_router_t::child_rx_ctx_t;

    /** @brief The live receiver context registered under @p name, or null. */
    static const ctx_t* ctx_by_name(const fwd_router_t& r, std::string_view name) {
        return r.ctx_by_name(name);
    }

    /** @brief `hop_mint` for @p name, or for @p ctx when it is not null. */
    static std::optional<wire::path_ref_element_t> hop_mint(const fwd_router_t& r,
                                                            std::string_view name,
                                                            const ctx_t* ctx) {
        return r.hop_mint(name, ctx);
    }
};
}  // namespace tr::net

namespace {

using door_t = tr::net::fwd_router_test_door_t;
using tr::testing::check;

/** @brief A transport that drops what it is handed — the mint never sends. */
class null_link_t : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte>) override {}
};

/** @brief The child the test registers and removes. */
constexpr std::string_view kMount = "net/mod/a";

/** @brief A removed child mints no reference, by name or through its retired context. */
void test_removed_child_mints_nothing() {
    std::printf("hop_mint refuses a removed child\n");
    tr::graph::graph_t g;
    tr::net::fwd_router_t router(g);
    null_link_t link;

    check(router.attach_link(std::string(kMount), link), "registered the child");
    const door_t::ctx_t* const ctx = door_t::ctx_by_name(router, kMount);
    check(ctx != nullptr, "a live child has a receiver context");
    check(door_t::hop_mint(router, kMount, nullptr).has_value(), "a LIVE child mints by name");
    check(door_t::hop_mint(router, {}, ctx).has_value(), "a LIVE child mints through its ctx");

    check(router.remove_child(kMount), "removed it");
    check(door_t::ctx_by_name(router, kMount) == nullptr, "ctx_by_name skips the retired context");
    check(!door_t::hop_mint(router, kMount, nullptr).has_value(),
          "a REMOVED child mints nothing by name");
    check(!door_t::hop_mint(router, kMount, ctx).has_value(),
          "a REMOVED child mints nothing through the context it left behind");
}

}  // namespace

int main() {
    test_removed_child_mints_nothing();
    return tr::testing::summary("hop_mint_tombstone");
}
