/**
 * @file
 * @brief #2117 — every reclaim refusal is answered and counted, and a link teardown cannot be
 *        refused its park.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A retirement the graph cannot park is a leak by design: freeing what a reader may still
 * hold would be a use-after-free. Before #2117 one of those leaks was silent: a refused
 * `graph_t::park_release` at link teardown kept the link for the graph's lifetime, and both
 * callers discarded the status. This suite drives each refusal through the graph's interface
 * on a source that refuses on demand, and checks the caller is answered `BACKPRESSURE` and the
 * matching `graph_t::reclaim_refusals` counter moves:
 *
 *   (a) `park_release` and `reserve_park` on a park that cannot grow — `park_refused`;
 *       a reserved park cannot be refused, and `collect()` runs it;
 *   (b) `retire` with no room for its key or its seam — `retire_refused`;
 *   (c) a link teardown, swept over every budget the source could have left: the park is
 *       never refused (its entry was reserved when the connection was created), and every
 *       removal that completes gives back every block it drew;
 *   (d) a creation (a module's endpoint, a connection) swept over every budget: a refusal,
 *       the park reservation's among them, answers `BACKPRESSURE` and creates nothing;
 *   (e) a null release hook handed to `park_release` / `park_reserved` parks nothing, so
 *       `collect()` never frees the caller's context into the table source as a seam.
 *
 * The unsubscribe refusal (`release_refused`) is reclaim_test's (f).
 */

#include <cstdio>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::testing::check;

/**
 * @brief A source that serves from the heap until armed, then serves `allow` more blocks and
 *        refuses every request after them. It records every live block, so a leak is a
 *        number the test asserts, and frees what is left when it goes, so a block a case
 *        keeps on purpose is not reported a second time by LeakSanitizer.
 */
class gate_source_t final : public tr::mem::block_source_t {
   public:
    gate_source_t() noexcept : tr::mem::block_source_t("gate") {}
    gate_source_t(const gate_source_t&) = delete;
    gate_source_t& operator=(const gate_source_t&) = delete;
    ~gate_source_t() {
        for (const auto& [p, b] : live_) ::operator delete(p, b.first, std::align_val_t{b.second});
    }

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_ && allow_ == 0) {
            ++refused_;
            return nullptr;
        }
        if (armed_) --allow_;
        void* const p = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
        if (p != nullptr) live_.emplace(p, std::pair{bytes, align});
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        live_.erase(p);
        ::operator delete(p, bytes, std::align_val_t{align});
    }
    [[nodiscard]] tr::mem::source_stats_t stats() const noexcept override {
        tr::mem::source_stats_t s;
        s.refused = refused_;
        return s;
    }

    /** @brief Serve @p allow more blocks, then refuse. */
    void arm(std::size_t allow) noexcept {
        armed_ = true;
        allow_ = allow;
        refused_ = 0;
    }
    /** @brief Serve everything again. */
    void disarm() noexcept { armed_ = false; }
    /** @brief Blocks handed out and not given back. */
    [[nodiscard]] std::size_t live() const noexcept { return live_.size(); }

    std::size_t refused_ = 0; /**< @brief Requests refused since the last @ref arm. */

   private:
    bool armed_ = false;
    std::size_t allow_ = 0;
    std::unordered_map<void*, std::pair<std::size_t, std::size_t>> live_;
};

/** @brief A release that counts its runs. */
void count_release(void* c) { ++*static_cast<int*>(c); }

/** @brief An inert `on_read`, so a vertex bears a value seam that retirement parks. */
tr::graph::result_t<tr::graph::value_ref_t> inert_read() {
    return std::unexpected(status_t::NOT_FOUND);
}

/** @brief Register @p path as a HANDLER bearing a value seam. */
vertex_handle_t seam_vertex(graph_t& g, const char* path) {
    handlers_t h;
    auto on_read = [] { return inert_read(); };
    h.on_read = tr::graph::thunk(on_read);
    return g.register_vertex(path_t(path), role_t::HANDLER, std::move(h));
}

void test_park_refusals() {
    std::printf("#2117 (a): a park that cannot grow answers BACKPRESSURE and counts:\n");
    gate_source_t src;
    {
        graph_t g{src};
        int runs = 0;
        src.arm(0);
        const auto parked = g.park_release({&runs, &count_release});
        check(!parked && parked.error() == status_t::BACKPRESSURE,
              "park_release on a park that cannot grow answers BACKPRESSURE");
        const auto reserved = g.reserve_park();
        check(!reserved && reserved.error() == status_t::BACKPRESSURE, "so does reserve_park");
        check(g.reclaim_refusals().park_refused == 2, "each is counted in park_refused");
        src.disarm();
        g.collect();
        check(runs == 0, "and the refused release is never run");

        check(g.reserve_park().has_value(), "with room, reserve_park succeeds");
        src.arm(0);
        g.park_reserved({&runs, &count_release});
        check(src.refused_ == 0,
              "a reserved park draws nothing, so the dry source refuses nothing");
        src.disarm();
        g.collect();
        check(runs == 1, "and collect() runs the reserved release once");
        check(g.reclaim_refusals().park_refused == 2, "a reserved park is never a refusal");

        check(g.reserve_park().has_value(), "a reservation given back ...");
        g.unreserve_park();
        check(g.park_release({&runs, &count_release}).has_value(), "... leaves the park usable");
        g.collect();
        check(runs == 2, "and its release runs at the next collect()");
    }
    check(src.live() == 0, "the graph gave back every block it drew");
}

void test_retire_refusal() {
    std::printf("#2117 (b): a retire with no room answers BACKPRESSURE and counts:\n");
    gate_source_t src;
    {
        graph_t g{src};
        const vertex_handle_t v = seam_vertex(g, "/dev/h");
        src.arm(0);
        const auto r = g.retire(v);
        check(!r && r.error() == status_t::BACKPRESSURE, "retire answers BACKPRESSURE");
        check(g.reclaim_refusals().retire_refused == 1, "and is counted in retire_refused");
        src.disarm();
        check(g.find(path_t("/dev/h").key()).has_value(), "nothing was retired");
        check(g.retire(v).has_value(), "with room again the retire succeeds");
        check(g.reclaim_refusals().retire_refused == 1, "and counts nothing");
        g.collect();
    }
    check(src.live() == 0, "the graph gave back every block it drew");
}

/** @brief A point-to-point link that records nothing and opens nothing. */
class p2p_link_t : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> frame) override { (void)frame; }
};

/** @brief A bus-capable link: its connection vertex bears an `on_children` seam whose
 *         context is the connection's park block. */
class bus_capable_link_t final : public tr::net::transport_t, public tr::net::bus_link_t {
   public:
    void send(std::span<const std::byte> frame) override { (void)frame; }
    [[nodiscard]] tr::net::bus_link_t* bus() override { return this; }
    void enumerate_peers(const peer_visitor_t& visit) const override { visit("p0"); }
    [[nodiscard]] tr::net::transport_t* peer_link(std::string_view peer) override {
        return peer == "p0" ? this : nullptr;
    }
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer,
                                             std::span<char>) const override {
        return peer.valid() ? "p0" : std::string_view{};
    }
};

/** @brief Create `/net/ws-client/<name>` over the staged @p link through the creator endpoint. */
bool create_conn(graph_t& g, transport_vertex_t& net, tr::net::transport_t& link,
                 std::string_view name) {
    if (!net.register_module("ws-client", "", tr::net::conn_role_t::DIAL)) return false;
    net.provide_link("ws-client", std::string(name), link);
    const auto p = path_t::parse("/net/ws-client/conn");
    return p && g.write(*p, tr::net::conn_spec_t(name).view()).has_value();
}

/** @brief What one teardown sweep found. */
struct sweep_t {
    bool park_never_refused = true; /**< @brief No teardown met a park refusal. */
    bool refusal_answered = true;   /**< @brief Every refused removal said so. */
    bool no_leak = true;            /**< @brief Every completed removal gave back every block. */
    bool completed = false;         /**< @brief Some budget sufficed. */
    std::size_t refused = 0;        /**< @brief Budgets at which the removal was refused. */
};

/** @brief Tear one connection over @p link down at every budget the source could have left. */
template <class Link>
sweep_t sweep_teardown() {
    sweep_t s;
    for (std::size_t allow = 0; allow < 512 && !s.completed; ++allow) {
        gate_source_t src;
        bool removed = false;
        {
            graph_t g{src};
            {
                fwd_router_t router(g);
                transport_vertex_t net(g, router);
                Link link;
                if (!create_conn(g, net, link, "c")) return s;
                src.arm(allow);
                const auto r = net.remove_connection("net/ws-client/c");
                src.disarm();
                const graph_t::reclaim_refusals_t n = g.reclaim_refusals();
                s.park_never_refused = s.park_never_refused && n.park_refused == 0;
                removed = r.has_value();
                if (!removed) {
                    ++s.refused;
                    s.refusal_answered = s.refusal_answered && r.error() == status_t::BACKPRESSURE;
                }
                g.collect();
            }
            g.collect();
        }
        // A refused retire keeps the link for the graph's lifetime by design (the gate frees
        // that block itself); a removal that completed must have given back everything.
        if (removed) {
            s.no_leak = s.no_leak && src.live() == 0;
            s.completed = true;
        }
    }
    return s;
}

void test_link_teardown_cannot_be_refused_its_park() {
    std::printf("#2117 (c): a link teardown under a dry source is never refused its park:\n");
    const sweep_t p2p = sweep_teardown<p2p_link_t>();
    check(p2p.completed, "point-to-point: some budget lets the removal complete");
    check(p2p.park_never_refused, "point-to-point: the park never refused a teardown");
    check(p2p.refusal_answered, "point-to-point: every refused removal answered BACKPRESSURE");
    check(p2p.no_leak, "point-to-point: every completed removal leaked nothing");
    const sweep_t bus = sweep_teardown<bus_capable_link_t>();
    check(bus.completed, "bus: some budget lets the removal complete");
    check(bus.park_never_refused, "bus: the park never refused a teardown");
    check(bus.refusal_answered, "bus: every refused removal answered BACKPRESSURE");
    check(bus.no_leak, "bus: every completed removal leaked nothing");
}

/** @brief Sweep one creation over every budget: each refusal answers `BACKPRESSURE` and
 *         creates nothing, and some refusal is the park reservation's own. */
template <class Create, class Exists>
void sweep_creation(const char* what, Create create, Exists exists) {
    bool answered = true, nothing_created = true, reservation_refused = false, done = false;
    for (std::size_t allow = 0; allow < 512 && !done; ++allow) {
        gate_source_t src;
        {
            graph_t g{src};
            fwd_router_t router(g);
            transport_vertex_t net(g, router);
            p2p_link_t link;
            const auto r = create(g, net, link, src, allow);
            if (r) {
                done = true;
                continue;
            }
            answered = answered && r.error() == status_t::BACKPRESSURE;
            nothing_created = nothing_created && !exists(g);
            reservation_refused = reservation_refused || g.reclaim_refusals().park_refused != 0;
        }
        nothing_created = nothing_created && src.live() == 0;
    }
    std::printf("  %s:\n", what);
    check(done, "some budget lets the creation complete");
    check(answered, "every refused creation answered BACKPRESSURE");
    check(nothing_created, "every refused creation created nothing and leaked nothing");
    check(reservation_refused, "some refusal was the park reservation's");
}

/** @brief Whether @p path names a vertex in @p g. */
bool exists_at(graph_t& g, const char* path) {
    const auto p = path_t::parse(path);
    return p && g.find(p->key()).has_value();
}

void test_creation_refused_its_reservation() {
    std::printf("#2118 (d): a creation whose park entry cannot be reserved is refused whole:\n");
    sweep_creation(
        "a module declaration (its creator endpoint)",
        [](graph_t&, transport_vertex_t& net, p2p_link_t&, gate_source_t& src, std::size_t allow) {
            src.arm(allow);
            const auto r = net.register_module("ws-client", "", tr::net::conn_role_t::DIAL);
            src.disarm();
            return r;
        },
        [](graph_t& g) { return exists_at(g, "/net/ws-client/conn"); });
    sweep_creation(
        "a connection",
        [](graph_t& g, transport_vertex_t& net, p2p_link_t& link, gate_source_t& src,
           std::size_t allow) -> tr::graph::result_t<void> {
            if (!net.register_module("ws-client", "", tr::net::conn_role_t::DIAL))
                return std::unexpected(status_t::NOT_FOUND);
            net.provide_link("ws-client", "c", link);
            const auto p = path_t::parse("/net/ws-client/conn");
            src.arm(allow);
            const auto r = g.write(*p, tr::net::conn_spec_t("c").view());
            src.disarm();
            return r;
        },
        [](graph_t& g) { return exists_at(g, "/net/ws-client/c"); });
}

void test_null_hook_is_never_freed() {
    std::printf("#2118 (e): a null release hook parks nothing and is never freed as a seam:\n");
    gate_source_t src;
    {
        graph_t g{src};
        int not_from_the_pool = 0;
        check(g.park_release({&not_from_the_pool, nullptr}).has_value(),
              "park_release with a null hook answers {}");
        check(g.reserve_park().has_value(), "a reservation ...");
        g.park_reserved({&not_from_the_pool, nullptr});
        check(g.parked_seam_count() == 0, "... and park_reserved with one: neither parks a seam");
        const std::size_t live = src.live();
        g.collect();
        check(src.live() == live, "collect() frees nothing into the table source");
    }
    check(src.live() == 0, "the graph gave back every block it drew");
}

}  // namespace

int main() {
    test_park_refusals();
    test_retire_refusal();
    test_link_teardown_cannot_be_refused_its_park();
    test_creation_refused_its_reservation();
    test_null_hook_is_never_freed();
    return tr::testing::summary("reclaim_refusal");
}
