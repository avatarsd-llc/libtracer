/**
 * @file
 * @brief Test-side adapters that hand a CAPTURING lambda to `graph_t`'s `{fn, ctx}`
 *        configuration seams (#1049).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `graph_hooks_t::remote_delivery` / `graph_hooks_t::subscription_observer` take the ADR-0047 pair
 * — a bare function pointer plus an opaque context — because a `std::function` cannot be published
 * coherently to a racing reader: assigning one DESTROYS the old target, so a setter racing a
 * hot-path reader freed the captured state that reader was standing on (#1049). Production installs
 * a captureless thunk over a long-lived object (`fwd_router_t` passes `this`), which is exactly
 * what the shape is for.
 *
 * Tests, though, overwhelmingly want a lambda that captures a counter on the stack. These
 * guards are that: a named local that OWNS the callable, installs a captureless thunk over
 * itself, and lives to the end of the enclosing scope. The ownership is the point — it is the
 * lifetime obligation the seam states, made local and visible, rather than a `std::function`
 * smuggled back into the graph. Immovable and non-copyable on purpose: the installed context
 * is `this`, so a moved guard would leave the graph pointing at a dead object.
 *
 * Beside them: the subject-seam and core-array conveniences the graph tests share
 * (`subject_into`, `read_subscribers`, `drain_unflushed`).
 *
 * `tr::testing` is a tests-only namespace — it is not a layer in the L0..L5 model and nothing
 * under `core/src` or `core/include` may name it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/view.hpp"

namespace tr::testing {

/** @brief Write @p token into a subject seam's caller buffer — the one step every test subject
 *         seam ends in (@ref tr::graph::subject_lookup_fn_t). A refused append fails closed. */
inline std::expected<void, tr::wire::err_t> subject_into(tr::mem::bytes_t& out,
                                                         std::span<const std::byte> token) {
    if (!out.append(token.data(), token.size()))
        return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    return {};
}

/** @brief @ref subject_into over a text token. */
inline std::expected<void, tr::wire::err_t> subject_into(tr::mem::bytes_t& out,
                                                         std::string_view token) {
    return subject_into(out, std::as_bytes(std::span(token.data(), token.size())));
}

/** @brief The subject seam most tests install: the caller context IS the subject token. */
inline std::expected<void, tr::wire::err_t> caller_is_subject_into(void* /*ctx*/,
                                                                   std::string_view caller,
                                                                   tr::mem::bytes_t& out) {
    return subject_into(out, caller);
}

/** @brief `g.read_subscribers` copied into a `std::vector` — a test convenience over the
 *         core-array form the library serves. */
inline tr::graph::result_t<std::vector<tr::view::view_t>> read_subscribers(
    const tr::graph::graph_t& g, tr::graph::vertex_handle_t v, std::string_view caller = {}) {
    tr::mem::block_array_t<tr::view::view_t> out(tr::mem::heap_source());
    const tr::graph::result_t<std::size_t> n = g.read_subscribers(v, out, caller);
    if (!n) return std::unexpected(n.error());
    return std::vector<tr::view::view_t>(out.begin(), out.end());
}

/** @brief `g.drain_unflushed` into a `std::vector` (overwritten on success) — a test
 *         convenience over the core-array form the library serves. */
inline tr::graph::result_t<std::size_t> drain_unflushed(tr::graph::graph_t& g,
                                                        tr::graph::vertex_handle_t v,
                                                        std::vector<tr::graph::value_ref_t>& out,
                                                        std::uint64_t* gap_before = nullptr) {
    tr::mem::block_array_t<tr::graph::value_ref_t> got(tr::mem::heap_source());
    const tr::graph::result_t<std::size_t> n = g.drain_unflushed(v, got, gap_before);
    if (n) out.assign(got.begin(), got.end());
    return n;
}

/**
 * @brief The pairs a test with no transport plane admits a remote edge over @p name with
 *        (#1941) — what the router would mint, without the router.
 *
 * The graph stores a remote edge's link as pairs and never dereferences them, so any value
 * works as long as two names get two links and one name always gets the same: the index is
 * the name's FNV-1a hash (top bit clear, so it is never `link_pair_t::kNoConn`). A sink compares
 * `d.link == test_link("a")`, and an eviction passes the same value beside the name.
 */
constexpr tr::graph::link_pair_t test_link(std::string_view name) noexcept {
    std::uint32_t h = 2166136261u;
    for (const char c : name) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return tr::graph::link_pair_t{.conn = h & 0x7FFFFFFFu, .peer = {}};
}

/**
 * @brief Install on @p g the link-name answer a transport plane would (#1941): each of
 *        @p names for its @ref test_link pair. Only a build with the session seam stores the
 *        hook; elsewhere the install stores nothing. @p names must outlive @p g's use of it.
 */
inline void name_test_links(tr::graph::graph_t& g, const std::vector<std::string>& names) {
    g.set_hook(&tr::graph::graph_hooks_t::link_name,
               {[](void* ctx, tr::graph::link_pair_t link, std::span<char>) -> std::string_view {
                    for (const std::string& n : *static_cast<const std::vector<std::string>*>(ctx))
                        if (test_link(n) == link) return n;
                    return {};
                },
                const_cast<std::vector<std::string>*>(&names)});
}

/** @brief `g.evict_link_edges(name, test_link(name))` — a departure in a router-less test. */
inline std::size_t evict_test_link(tr::graph::graph_t& g, std::string_view name) {
    return g.evict_link_edges(name, test_link(name));
}

/**
 * @brief Install on @p r the link supplier a transport plane would (#1941): every inbound link
 *        NAME answers its @ref test_link pairs, and no index token.
 *
 * A resolver with no supplier admits no remote subscriber, because the edge would have nowhere
 * to deliver; a router-less test that drives remote subscribes through the resolver installs
 * this instead of a router.
 */
inline void carry_test_links(tr::graph::op_resolver_t& r) {
    r.on_link_id(
        [](void*, const tr::graph::inbound_ref_t& inbound) {
            return tr::graph::carried_link_t{.link = test_link(inbound.link)};
        },
        nullptr);
}

/** @brief Owns a capturing remote-delivery sink and keeps it installed for its own lifetime. */
template <typename F>
class remote_sink_guard_t {
   public:
    /** @brief Install @p f on @p g; it stays installed until this guard dies. */
    remote_sink_guard_t(tr::graph::graph_t& g, F f) : fn_(std::move(f)) {
        g.set_hook(
            &tr::graph::graph_hooks_t::remote_delivery,
            {[](void* ctx, const tr::graph::remote_delivery_t& d, const tr::graph::value_t& v) {
                 static_cast<remote_sink_guard_t*>(ctx)->fn_(d, v);
             },
             this});
    }
    remote_sink_guard_t(const remote_sink_guard_t&) = delete;            /**< @brief Immovable. */
    remote_sink_guard_t& operator=(const remote_sink_guard_t&) = delete; /**< @brief Immovable. */

   private:
    F fn_; /**< @brief The owned callable the installed thunk forwards to. */
};

/** @brief CTAD so a call site spells `const remote_sink_guard_t sink(g, [&]{…});`. */
template <typename F>
remote_sink_guard_t(tr::graph::graph_t&, F) -> remote_sink_guard_t<F>;

/** @brief Owns a capturing subscription observer and keeps it installed for its own lifetime. */
template <typename F>
class sub_observer_guard_t {
   public:
    /** @brief Install @p f on @p g; it stays installed until this guard dies. */
    sub_observer_guard_t(tr::graph::graph_t& g, F f) : fn_(std::move(f)) {
        g.set_hook(&tr::graph::graph_hooks_t::subscription_observer,
                   {[](void* ctx, const tr::graph::sub_event_t& e) {
                        static_cast<sub_observer_guard_t*>(ctx)->fn_(e);
                    },
                    this});
    }
    sub_observer_guard_t(const sub_observer_guard_t&) = delete;            /**< @brief Immovable. */
    sub_observer_guard_t& operator=(const sub_observer_guard_t&) = delete; /**< @brief Immovable. */

   private:
    F fn_; /**< @brief The owned callable the installed thunk forwards to. */
};

/** @brief CTAD so a call site spells `const sub_observer_guard_t obs(g, [&]{…});`. */
template <typename F>
sub_observer_guard_t(tr::graph::graph_t&, F) -> sub_observer_guard_t<F>;

/**
 * @brief A creation hook (RFC-0030 §7.2) that makes every missing child of its parent a plain
 *        `STORED_VALUE` vertex — what a test installs to keep "a write creates" below one
 *        parent. `ctx` is the graph.
 */
inline tr::graph::result_t<void> create_stored_value(void* ctx, tr::graph::vertex_handle_t,
                                                     std::span<const std::byte> child_key,
                                                     std::string_view, const tr::view::rope_t&) {
    auto& g = *static_cast<tr::graph::graph_t*>(ctx);
    const auto made =
        g.register_vertex_key(std::vector<std::byte>(child_key.begin(), child_key.end()),
                              tr::graph::role_t::STORED_VALUE);
    if (!made) return std::unexpected(made.error());
    return {};
}

/**
 * @brief Install @ref create_stored_value on @p parent.
 * @retval false The build has no creation-hook slot (`kCreationHooks` off), or the install
 *         was refused; a caller gates the assertions that need creation on it.
 */
inline bool allow_creation(tr::graph::graph_t& g, tr::graph::vertex_handle_t parent) {
    return g.set_creation_hook(parent, {&create_stored_value, &g}).has_value();
}

}  // namespace tr::testing
