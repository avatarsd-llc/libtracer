/**
 * @file
 * @brief The session-admission seam (#1841): a per-session subscribe check before the edge is
 *        added, and a per-session teardown event.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Before this, an embedder could not hold each session to its own subscription budget: the
 * subscription observer runs after the edge is admitted and is keyed by the caller context,
 * which collapses to the bound subject, so every session of one subject shared one key. Here
 * an embedder installs a budget of `kBudget` subscriptions per session, entirely in its own
 * state, and:
 *
 * 1. a session that subscribes `kBudget + 1` times has the last refused with `BACKPRESSURE`,
 *    and the first `kBudget` stay — each one still delivers;
 * 2. a second session of the SAME subject gets its own `kBudget`;
 * 3. the session's departure (`evict_link_edges`, which `fwd_router_t::link_down` and a bus
 *    peer's departure both run) fires one `END`, the embedder refunds all `kBudget` there, and
 *    the session can subscribe again;
 * 4. every edge that ends one at a time gives its charge back with one `RELEASE`: an admission
 *    the graph then refused (a `:subscribers[N]` replace naming no slot), a clear, a replace's
 *    displaced edge, a route refusal and the producer's retirement — so subscribe/unsubscribe
 *    churn and repeated `[0]` replaces never exhaust the budget;
 * 5. a local subscription has no session and is never offered;
 * 6. with no hook installed every subscription is admitted, exactly as before.
 *
 * Every vector fails with the production change reverted: the seam does not exist, so this file
 * does not compile. In a build without `kSessionAdmission` the vectors are skipped, and the
 * `static_assert` below pins that installing the hook there does not compile. The no-hook build's
 * cost is a compile-time claim (`config_t:: kSessionAdmission` off folds every read away) and
 * `edge_view_t`'s size is pinned by the `static_assert` in subscriber.hpp, which this change leaves
 * alone.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::field_path_t;
using tr::graph::field_step_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::session_event_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief The per-session budget the embedder enforces. */
constexpr int kBudget = 3;

/** @brief A SUBSCRIBER TLV whose PATH names @p marker. */
std::vector<std::byte> b_subscriber(std::string_view marker) {
    std::vector<std::byte> path_body;
    (void)tr::wire::emit_path_segment(path_body, marker);
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, path_body);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

/** @brief A PATH TLV naming @p link — the return route a wire subscribe arrives with. */
std::vector<std::byte> b_route(std::string_view link) {
    std::vector<std::byte> body;
    (void)tr::wire::emit_path_segment(body, link);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/**
 * @brief One wire subscribe at @p v over session @p link, as subject @p subject.
 * @return The admission's status; `nullopt` for an admitted subscription.
 */
std::optional<status_t> wire_sub(graph_t& g, vertex_handle_t v, std::string_view link,
                                 std::string_view subject) {
    const auto r = g.subscribe_wire(v, make_value(b_subscriber("t")), make_value(b_route(link)),
                                    link, {}, subject);
    if (r) return std::nullopt;
    return r.error();
}

/** @brief The embedder: a per-session budget kept in its own state, and what it was told. */
struct budget_t {
    std::map<std::string, int> charged; /**< @brief Charges per session. */
    std::vector<std::string> ends;      /**< @brief Every `END`'s session, in order. */
    std::vector<std::string> callers;   /**< @brief Every `ADMIT`'s caller, in order. */
    std::vector<std::string> producers; /**< @brief Every `ADMIT`'s producer key, in order. */
    int underflows = 0;                 /**< @brief `RELEASE`s with nothing charged. */
    int releases = 0;                   /**< @brief How many `RELEASE`s arrived. */

    /** @brief The installed hook. */
    static bool hook(void* ctx, const session_event_t& e) {
        auto& self = *static_cast<budget_t*>(ctx);
        const std::string session(e.session);
        switch (e.kind) {
            case session_event_t::kind_t::ADMIT:
                self.callers.emplace_back(e.caller);
                self.producers.emplace_back(
                    reinterpret_cast<const char*>(e.producer.bytes().data()),
                    e.producer.bytes().size());
                if (self.charged[session] >= kBudget) return false;
                ++self.charged[session];
                return true;
            case session_event_t::kind_t::RELEASE:
                // Floored at zero, as the contract asks: a RELEASE may trail its session's END.
                ++self.releases;
                if (self.charged[session] == 0)
                    ++self.underflows;
                else
                    --self.charged[session];
                return true;
            case session_event_t::kind_t::END:
                self.ends.push_back(session);
                self.charged.erase(session);
                return true;
        }
        return true;
    }
};

/** @brief Whether @p H's session-admission member has a `fn` to install. */
template <class H>
concept has_session_hook = requires(H h) { h.session_admission.fn; };

/** @brief A build without the seam has no `fn` to install: naming it does not compile. */
static_assert(tr::graph::kSessionAdmission == has_session_hook<tr::graph::graph_hooks_t>,
              "graph_hooks_t::session_admission has a fn exactly when kSessionAdmission is on");

/**
 * @brief Install @p b's hook on @p g (or clear the seam, for null).
 *
 * The member is only a `{fn, ctx}` pair when the build has the seam, so the assignment sits in
 * a generic lambda's `if constexpr`: it is compiled where the member has a `fn`, and this file
 * still builds (and skips) where it does not.
 */
void install(graph_t& g, budget_t* b) {
    auto hooks = g.hooks();
    [b](auto& hook) {
        if constexpr (requires { hook.fn; }) hook = {b != nullptr ? &budget_t::hook : nullptr, b};
    }(hooks.session_admission);
    g.set_hooks(hooks);
}

/** @brief An empty `STATUS` — the `:subscribers[N]` clear sentinel. */
std::vector<std::byte> b_clear() {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::STATUS, opt_t{}, std::span<const std::byte>{});
    return out;
}

/** @brief The `:subscribers[N]` field, or the `:subscribers[]` append for no @p slot. */
field_path_t subscribers(std::optional<std::uint16_t> slot) {
    field_path_t f;
    f.steps.push_back(field_step_t{.name = "subscribers",
                                   .indexed = true,
                                   .append = !slot.has_value(),
                                   .index = slot.value_or(0)});
    return f;
}

/** @brief Vectors 1-3 and 6: the budget per session, its refund at departure, and no hook. */
void test_budget_per_session() {
    graph_t g;
    const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    std::map<std::string, int> delivered;
    const tr::testing::remote_sink_guard_t sink(
        g, [&](const tr::graph::remote_delivery_t& d, const tr::graph::value_t&) {
            ++delivered[std::string(d.link)];
        });
    budget_t budget;
    install(g, &budget);

    // 1. kBudget admitted on one session, the next refused, the first kBudget stay.
    for (int i = 0; i < kBudget; ++i)
        check(!wire_sub(g, p, "s1", "alice"), "session s1: a subscription within budget");
    check(wire_sub(g, p, "s1", "alice") == status_t::BACKPRESSURE,
          "session s1: the subscription past the budget is refused BACKPRESSURE");
    check(budget.charged["s1"] == kBudget, "the refusal charged nothing");
    check(!budget.callers.empty() && budget.callers.back() == "alice",
          "the check sees the subject as the caller");
    check(!budget.producers.empty() && !budget.producers.back().empty(),
          "the check sees the producer's key");

    // 2. The same subject on a second session has its own budget.
    for (int i = 0; i < kBudget; ++i)
        check(!wire_sub(g, p, "s2", "alice"), "session s2 of the same subject: its own budget");
    check(wire_sub(g, p, "s2", "alice") == status_t::BACKPRESSURE,
          "session s2: past its own budget is refused");

    check(g.write(p, make_value({0x01})).has_value(), "write the producer");
    check(delivered["s1"] == kBudget, "s1's admitted subscriptions all stayed and deliver");
    check(delivered["s2"] == kBudget, "s2's admitted subscriptions all stayed and deliver");

    // 3. The session departs: one END, the whole charge refunded, and the budget is whole again.
    check(g.evict_link_edges("s1") == static_cast<std::size_t>(kBudget),
          "s1's departure evicts its edges");
    check(budget.ends == std::vector<std::string>{"s1"}, "one END, for the departed session");
    check(budget.charged.count("s1") == 0, "the END refunded all of s1's charges");
    check(budget.charged["s2"] == kBudget, "s2's charges are untouched");
    for (int i = 0; i < kBudget; ++i)
        check(!wire_sub(g, p, "s1", "alice"), "s1's name subscribes again after the refund");
    check(budget.releases == 0, "nothing was released: every admitted edge landed");

    // 6. No hook: the seam admits everything, as before #1841.
    install(g, nullptr);
    for (int i = 0; i < kBudget + 2; ++i)
        check(!wire_sub(g, p, "s3", "alice"), "with no hook every subscription is admitted");
    check(budget.charged.count("s3") == 0, "an uninstalled hook is told nothing");
    (void)g.evict_link_edges("s3");
    check(budget.ends.size() == 1, "nor of a departure");
}

/** @brief Vector 4: a granted admission whose slot verb refuses is RELEASEd. */
void test_refused_landing_is_released() {
    graph_t g;
    const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
    budget_t budget;
    install(g, &budget);

    // A `:subscribers[99]` replace from a peer: admitted by the hook under its session (the
    // caller context, since this door binds no link), then refused for naming no slot.
    field_path_t f;
    f.steps.push_back(field_step_t{.name = "subscribers", .indexed = true, .index = 99});
    const auto r = g.write(p, f, tr::view::rope_t{make_value(b_subscriber("t"))}, "peer");
    check(!r && r.error() == status_t::INVALID_PATH, "the replace naming no slot is refused");
    check(budget.callers.size() == 1, "the replace was offered to the hook");
    check(budget.releases == 1, "and its charge released when it did not land");
    check(budget.charged["peer"] == 0, "the embedder's charge is exact");
    check(budget.underflows == 0, "no RELEASE without its ADMIT");
}

/** @brief Vector 4: subscribe/unsubscribe churn never exhausts the budget. */
void test_churn_gives_back() {
    graph_t g;
    const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    budget_t budget;
    install(g, &budget);
    for (int i = 0; i < 2 * kBudget; ++i) {
        check(!wire_sub(g, p, "s1", "alice"), "churn: the subscribe is admitted");
        // The freed slot is reused, so the one live edge is always at [0].
        check(g.write(p, subscribers(0), tr::view::rope_t{make_value(b_clear())}, "s1").has_value(),
              "churn: the :subscribers[0] clear lands");
    }
    check(budget.charged["s1"] == 0, "churn: every clear gave its charge back");
    check(budget.releases == 2 * kBudget, "churn: one RELEASE per cleared edge");
    check(budget.underflows == 0, "churn: no RELEASE without its ADMIT");
}

/** @brief Vector 4: a `[0]` replace is net zero — the displaced edge is RELEASEd. */
void test_replace_is_net_zero() {
    graph_t g;
    const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
    budget_t budget;
    install(g, &budget);
    const auto sub = [] { return tr::view::rope_t{make_value(b_subscriber("t"))}; };
    check(g.write(p, subscribers(std::nullopt), sub(), "peer").has_value(),
          "replace: the first edge is appended");
    for (int i = 0; i < kBudget + 1; ++i)
        check(g.write(p, subscribers(0), sub(), "peer").has_value(),
              "replace: every [0] write is admitted");
    check(budget.charged["peer"] == 1, "replace: one live edge, one charge");
    check(budget.releases == kBudget + 1, "replace: each displaced edge was RELEASEd");
}

/** @brief Vector 4: a route refusal and the producer's retirement each give back per edge. */
void test_route_refusal_and_retire_give_back() {
    graph_t g;
    const vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    const vertex_handle_t q = g.register_vertex(path_t("/q"), role_t::STORED_VALUE);
    budget_t budget;
    install(g, &budget);

    for (int i = 0; i < kBudget; ++i) check(!wire_sub(g, p, "s1", "alice"), "route: admitted");
    const std::vector<std::byte> route = b_route("s1");
    check(g.evict_route_edges("s1", route) == static_cast<std::size_t>(kBudget),
          "route: the refusal reclaims the session's edges");
    check(budget.charged["s1"] == 0 && budget.releases == kBudget,
          "route: one RELEASE per reclaimed edge");

    for (int i = 0; i < kBudget; ++i) check(!wire_sub(g, q, "s1", "alice"), "retire: admitted");
    check(g.retire(q).has_value(), "retire: the producer retires");
    check(budget.charged["s1"] == 0 && budget.releases == 2 * kBudget,
          "retire: one RELEASE per edge the retirement dropped");
    check(budget.ends.empty(), "neither is a session departure");
    check(budget.underflows == 0, "no RELEASE without its ADMIT");
}

/** @brief Vector 5: a local subscription has no session and is never offered. */
void test_local_subscription_is_not_offered() {
    graph_t g;
    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    budget_t budget;
    install(g, &budget);
    for (int i = 0; i < kBudget + 2; ++i)
        check(g.subscribe(
                   path_t("/p"), [](void*, const tr::graph::value_t&) {}, nullptr)
                  .has_value(),
              "a local subscribe is admitted whatever the budget");
    check(budget.callers.empty(), "and never offered to the hook");
}

}  // namespace

int main() {
    if constexpr (!tr::graph::kSessionAdmission) {
        std::printf("  (skipped: this build binds kSessionAdmission = false)\n");
        return tr::testing::summary("session_admission");
    } else {
        test_budget_per_session();
        test_refused_landing_is_released();
        test_churn_gives_back();
        test_replace_is_net_zero();
        test_route_refusal_and_retire_give_back();
        test_local_subscription_is_not_offered();
        return tr::testing::summary("session_admission");
    }
}
