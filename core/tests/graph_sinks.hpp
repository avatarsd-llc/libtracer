/**
 * @file
 * @brief Test-side adapters that hand a CAPTURING lambda to `graph_t`'s `{fn, ctx}`
 *        configuration seams (#1049).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `graph_t::configure_remote_delivery_sink` / `configure_subject_resolver` /
 * `configure_subscription_observer` take the ADR-0047 pair — a bare function pointer plus an
 * opaque context — because a `std::function` cannot be published coherently to a racing
 * reader: assigning one DESTROYS the old target, so a setter racing a hot-path reader freed
 * the captured state that reader was standing on (#1049). Production installs a captureless
 * thunk over a long-lived object (`fwd_router_t` passes `this`), which is exactly what the
 * shape is for.
 *
 * Tests, though, overwhelmingly want a lambda that captures a counter on the stack. These
 * guards are that: a named local that OWNS the callable, installs a captureless thunk over
 * itself, and lives to the end of the enclosing scope. The ownership is the point — it is the
 * lifetime obligation the seam states, made local and visible, rather than a `std::function`
 * smuggled back into the graph. Immovable and non-copyable on purpose: the installed context
 * is `this`, so a moved guard would leave the graph pointing at a dead object.
 *
 * `tr::testing` is a tests-only namespace — it is not a layer in the L0..L5 model and nothing
 * under `core/src` or `core/include` may name it.
 */
#pragma once

#include <expected>
#include <string_view>
#include <utility>

#include "libtracer/graph.hpp"
#include "libtracer/view.hpp"

namespace tr::testing {

/** @brief Owns a capturing remote-delivery sink and keeps it installed for its own lifetime. */
template <typename F>
class remote_sink_guard_t {
   public:
    /** @brief Install @p f on @p g; it stays installed until this guard dies. */
    remote_sink_guard_t(tr::graph::graph_t& g, F f) : fn_(std::move(f)) {
        g.configure_remote_delivery_sink(
            [](void* ctx, const tr::graph::remote_delivery_t& d, const tr::view::rope_t& v) {
                static_cast<remote_sink_guard_t*>(ctx)->fn_(d, v);
            },
            this);
    }
    remote_sink_guard_t(const remote_sink_guard_t&) = delete;            /**< @brief Immovable. */
    remote_sink_guard_t& operator=(const remote_sink_guard_t&) = delete; /**< @brief Immovable. */

   private:
    F fn_; /**< @brief The owned callable the installed thunk forwards to. */
};

/** @brief CTAD so a call site spells `const remote_sink_guard_t sink(g, [&]{…});`. */
template <typename F>
remote_sink_guard_t(tr::graph::graph_t&, F) -> remote_sink_guard_t<F>;

/** @brief Owns a capturing subject resolver and keeps it installed for its own lifetime. */
template <typename F>
class subject_resolver_guard_t {
   public:
    /** @brief Install @p f on @p g; it stays installed until this guard dies. */
    subject_resolver_guard_t(tr::graph::graph_t& g, F f) : fn_(std::move(f)) {
        g.configure_subject_resolver(
            [](void* ctx, std::string_view caller)
                -> std::expected<tr::graph::subject_token_t, tr::wire::err_t> {
                return static_cast<subject_resolver_guard_t*>(ctx)->fn_(caller);
            },
            this);
    }
    subject_resolver_guard_t(const subject_resolver_guard_t&) = delete; /**< @brief Immovable. */
    /** @brief Immovable. */
    subject_resolver_guard_t& operator=(const subject_resolver_guard_t&) = delete;

   private:
    F fn_; /**< @brief The owned callable the installed thunk forwards to. */
};

/** @brief CTAD so a call site spells `const subject_resolver_guard_t r(g, [&]{…});`. */
template <typename F>
subject_resolver_guard_t(tr::graph::graph_t&, F) -> subject_resolver_guard_t<F>;

/** @brief Owns a capturing subscription observer and keeps it installed for its own lifetime. */
template <typename F>
class sub_observer_guard_t {
   public:
    /** @brief Install @p f on @p g; it stays installed until this guard dies. */
    sub_observer_guard_t(tr::graph::graph_t& g, F f) : fn_(std::move(f)) {
        g.configure_subscription_observer(
            [](void* ctx, const tr::graph::sub_event_t& e) {
                static_cast<sub_observer_guard_t*>(ctx)->fn_(e);
            },
            this);
    }
    sub_observer_guard_t(const sub_observer_guard_t&) = delete;            /**< @brief Immovable. */
    sub_observer_guard_t& operator=(const sub_observer_guard_t&) = delete; /**< @brief Immovable. */

   private:
    F fn_; /**< @brief The owned callable the installed thunk forwards to. */
};

/** @brief CTAD so a call site spells `const sub_observer_guard_t obs(g, [&]{…});`. */
template <typename F>
sub_observer_guard_t(tr::graph::graph_t&, F) -> sub_observer_guard_t<F>;

}  // namespace tr::testing
