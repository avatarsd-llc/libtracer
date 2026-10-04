/**
 * @file
 * @brief The originating link's transport-catalog identity — the `(kind, role)` pair a write
 *        carries into the admission context (#1650) — without the transport seam behind it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * WHY IT IS SPLIT OUT. A write that arrived over a link reaches the graph's admission seam
 * (`handlers_t::on_admit`) and handler seam (`handlers_t::on_write`) through
 * `tr::graph::write_ctx_t`, which is L4. `tr::net` is the transport plane above it
 * (core/STYLE.md: dependencies point up the layers only), so a graph header may not include
 * `transport_vertex.hpp`, where the catalog lives. It may include this one: one view, one
 * byte of role, and no transport type at all — the same reason `peer_handle.hpp` stands
 * alone. `transport_vertex.hpp` includes this, so every spelling of `tr::net::conn_role_t`
 * keeps compiling unchanged.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace tr::net {

/**
 * @brief The connection's transport-private role (ADR-0027 §default link direction).
 *
 * `DIAL` = this node opens the link (the consumer-dials default); `LISTEN` = this node
 * accepts. A config-constructed socket transport acts on it (bind vs. connect).
 */
enum class conn_role_t : std::uint8_t { DIAL = 0, LISTEN = 1 };

/**
 * @brief WHAT KIND of link a write arrived on: the transport-catalog `(kind, role)` pair of
 *        the connection that carried it (#1650).
 *
 * It is the same pair a module is declared under (`transport_vertex_t::register_module`), so
 * it answers "a session a `ws` listener accepted" apart from "a `tcp` link this node dialled"
 * without inferring either from a link's NAME. The subject (`write_ctx_t::subject`) says WHO
 * wrote; this says over WHAT; a policy that must treat the two differently reads both.
 *
 * Fixed ONCE per link, at registration (`fwd_router_t::add_child`), and interned by the
 * router for its own lifetime — a frame carries a pointer to it and pays nothing to find it.
 * The record holds NO kind-specific field: it is the catalog's universal key and nothing else
 * (the ADR-0043 §5 leanness ruling).
 *
 * @warning BORROWED, like everything in `write_ctx_t`: valid for the call that received it.
 *          Copy @ref kind out if it must outlive the call.
 */
struct link_kind_t {
    /** @brief The transport-catalog `kind` selector (`"ws"`, `"tcp"`, `"can"`, ...). */
    std::string_view kind;
    conn_role_t role = conn_role_t::DIAL; /**< @brief The role the link's module fixes. */

    /** @brief Does this link match catalog entry @p k in role @p r? */
    [[nodiscard]] constexpr bool is(std::string_view k, conn_role_t r) const noexcept {
        return role == r && kind == k;
    }
};

}  // namespace tr::net
