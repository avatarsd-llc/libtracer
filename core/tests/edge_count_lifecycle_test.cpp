/**
 * @file
 * @brief A vertex's subscriber count equals its live edges across clear, admit and retire, and a
 *        retired vertex carries no edges into a later registration (#2836).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Clear and admit change the count and the slot under one shared map hold, the hold that
 * suspend/resume and link eviction already take, so retirement (the unique hold) always sees a
 * count and a slot table that agree. An admission lands only on the registration it was decided
 * against: a vertex retired since, registered again or not, takes no edge.
 *
 *   1. Admit, clear and retire move the count, the published edges and a descendant's ancestor
 *      count together, and a clear through a handle from before the retire changes nothing.
 *   2. An append through a handle to a retired vertex lands nothing, so the next registration at
 *      that address starts with no edges and a write to it reaches no target.
 *   3. An append decided before a retire and a re-registration lands nothing on the new
 *      registration. The subject seam is the fixed point between the decision and the landing:
 *      this test's seam retires and re-registers the vertex once, from inside the admission.
 *   4. One thread admits and clears while another retires and re-registers. Every retired state
 *      the retirer looks at holds no edge and a zero count, and afterwards the count, the
 *      published edges and the descendant's ancestor count are all zero.
 *
 * Without the change, (2) and (3) fail outright: the append lands, and the next registration
 * inherits it.
 *
 * The iteration count of (4) defaults to @ref kIterations; a first argument overrides it.
 */

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
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
using tr::graph::subscription_t;
using tr::graph::value_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief Iterations of each thread in a default run of (4). A first argument overrides it. */
constexpr long kIterations = 20000;

/** @brief A subscriber that does nothing: (1) and (4) count edges, not deliveries. */
void sink(void*, const value_t&) {}

/** @brief The byte the vertex at @p p holds, or -1 when it holds no value. A delivery over a
 *         path-target edge terminates at the target: it stores there and goes no further. */
int held(const graph_t& g, const path_t& p) {
    const auto r = g.read(p);
    return r && !(*r)->only().bytes().empty() ? std::to_integer<int>((*r)->only().bytes()[0]) : -1;
}

/** @brief The raw vertex behind @p h, for the published-edge and ancestor counts. */
tr::graph::vertex_t& raw(vertex_handle_t h) { return *std::bit_cast<tr::graph::vertex_t*>(h); }

/** @brief The entries the vertex behind @p h has published for the fan-out to walk. */
std::size_t published(vertex_handle_t h) { return raw(h).published_edges(); }

/** @brief Do the count of @p v, its published edges and @p kid's ancestor count all read @p n? */
bool agree(const graph_t& g, vertex_handle_t v, vertex_handle_t kid, std::size_t n) {
    return g.own_subs(v) == n && published(v) == n && raw(kid).listeners_above() == n;
}

/** @brief A one-byte write payload. */
tr::view::view_t byte_value(std::uint8_t b) {
    const std::byte one[1] = {std::byte{b}};
    return make_value(one);
}

/** @brief The `:subscribers[]` append field. */
field_path_t append_field() {
    field_path_t field;
    (void)field.steps.push_back(
        field_step_t{.name = "subscribers", .indexed = true, .append = true});
    return field;
}

/** @brief A `SUBSCRIBER{PATH <target>}` record, the value an append writes. */
tr::view::view_t subscriber_record(const path_t& target) {
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{}, target.key());
    std::vector<std::byte> rec;
    tr::wire::emit_tlv(rec, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, body);
    return make_value(rec);
}

/** @brief (1) Admit, clear and retire keep the count equal to the live edges. */
void test_count_follows_edges() {
    std::printf("admit, clear and retire move the count and the edges together:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/c/a"), role_t::STORED_VALUE);
    const vertex_handle_t kid = g.register_vertex(path_t("/c/a/k"), role_t::STORED_VALUE);
    std::vector<subscription_t> subs;
    for (int i = 0; i < 3; ++i) {
        if (const auto s = g.subscribe(path_t("/c/a"), sink, nullptr)) subs.push_back(*s);
    }
    check(subs.size() == 3 && agree(g, v, kid, 3), "three admits: all three counts read 3");
    check(g.unsubscribe(subs[0]).has_value() && agree(g, v, kid, 2), "a clear: all three read 2");
    check(g.retire(v).has_value() && agree(g, v, kid, 0), "retire: all three read 0");
    check(!g.unsubscribe(subs[1]).has_value() && agree(g, v, kid, 0),
          "a clear through a handle from before the retire changes nothing");
    check(g.try_register_vertex(path_t("/c/a"), role_t::STORED_VALUE).has_value() &&
              agree(g, v, kid, 0),
          "the re-registration starts with no edges");
    check(g.subscribe(path_t("/c/a"), sink, nullptr).has_value() && agree(g, v, kid, 1),
          "a new admit counts exactly one");
}

/** @brief (2) A retired vertex takes no edge, so its next registration inherits none. */
void test_retired_vertex_takes_no_edge() {
    std::printf("a retired vertex takes no edge into its next registration:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/c/b"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/c/t"), role_t::STORED_VALUE);
    const field_path_t field = append_field();
    const tr::view::view_t record = subscriber_record(path_t("/c/t"));

    check(g.retire(v).has_value(), "retire /c/b");
    check(!g.write(v, field, record).has_value(),
          "an append through the retired vertex's handle is refused");
    check(g.own_subs(v) == 0 && published(v) == 0, "... and the retired vertex holds no edge");
    check(g.try_register_vertex(path_t("/c/b"), role_t::STORED_VALUE).has_value(), "re-register");
    check(g.own_subs(v) == 0 && published(v) == 0, "the re-registration starts with no edges");
    (void)g.write(v, byte_value(1));
    check(held(g, path_t("/c/t")) == -1, "... and a write to it reaches no target");

    check(g.write(v, field, record).has_value() && g.own_subs(v) == 1 && published(v) == 1,
          "an append after the re-registration lands");
    (void)g.write(v, byte_value(2));
    check(held(g, path_t("/c/t")) == 2, "... and delivers");
}

/** @brief The subject seam of (3): once armed, it retires and re-registers the vertex. */
struct reregister_t {
    graph_t* g = nullptr;       /**< @brief The graph the seam is installed on. */
    vertex_handle_t v;          /**< @brief The vertex it retires. */
    const char* path = nullptr; /**< @brief The address it registers again. */
    bool armed = false;         /**< @brief Retire and re-register on the next call. */
    int calls = 0;              /**< @brief Calls seen while armed or after. */
};

/** @brief The seam itself: the caller is the subject, as in most tests. */
std::expected<void, tr::wire::err_t> reregister_once(void* ctx, std::string_view caller,
                                                     tr::mem::bytes_t& out, std::uint64_t&) {
    auto* const r = static_cast<reregister_t*>(ctx);
    ++r->calls;
    if (r->armed) {
        r->armed = false;
        (void)r->g->retire(r->v);
        (void)r->g->try_register_vertex(path_t(r->path), role_t::STORED_VALUE);
    }
    return tr::testing::subject_into(out, caller);
}

/** @brief (3) An append decided before a retire lands nothing on the next registration. */
void test_decision_does_not_cross_a_retire() {
    std::printf("an append decided before a retire lands nothing on the next registration:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/c/d"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/c/u"), role_t::STORED_VALUE);
    const field_path_t field = append_field();
    const tr::view::view_t record = subscriber_record(path_t("/c/u"));
    reregister_t seam{.g = &g, .v = v, .path = "/c/d"};
    g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {reregister_once, &seam});

    const std::uint32_t generation = g.retire_generation(v);
    seam.armed = true;
    check(!g.write(v, field, record, "peer").has_value(),
          "an append whose vertex was retired and registered again under it is refused");
    check(
        seam.calls == 1 && g.retire_generation(v) == generation + 1 && g.find(path_t("/c/d").key()),
        "... the seam ran once, and /c/d is registered again, one generation on");
    check(g.own_subs(v) == 0 && published(v) == 0, "... and the new registration has no edges");
    (void)g.write(v, byte_value(1));
    check(held(g, path_t("/c/u")) == -1, "... and a write to it reaches no target");

    check(g.write(v, field, record, "peer").has_value() && g.own_subs(v) == 1 && published(v) == 1,
          "an append decided against the new registration lands");
    (void)g.write(v, byte_value(2));
    check(held(g, path_t("/c/u")) == 2, "... and delivers");
    g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {nullptr, nullptr});
}

/** @brief (4) Admit and clear on one thread, retire and re-register on another. */
void test_concurrent_lifecycle(long iterations) {
    std::printf("admit and clear on one thread, retire and re-register on another:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/c/h"), role_t::STORED_VALUE);
    const vertex_handle_t kid = g.register_vertex(path_t("/c/h/k"), role_t::STORED_VALUE);
    // A start barrier: neither thread begins until both are running.
    std::atomic<int> ready{0};
    const auto arrive = [&] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    };
    std::atomic<bool> admitter_done{false};
    long landed = 0;
    long retired_states = 0;
    long held = 0;  // retired states the retirer saw holding an edge or a count
    std::thread admitter([&] {
        arrive();
        // Up to three edges live at once, so a retire takes more than one; past its count the
        // thread keeps going until an admission has landed.
        std::vector<subscription_t> live;
        for (long i = 0; i < iterations || landed == 0; ++i) {
            if (const auto s = g.subscribe(path_t("/c/h"), sink, nullptr)) {
                ++landed;
                live.push_back(*s);
            }
            if (live.size() > 2) {
                (void)g.unsubscribe(live.front());
                live.erase(live.begin());
            }
        }
        for (const subscription_t& s : live) (void)g.unsubscribe(s);
        admitter_done.store(true, std::memory_order_release);
    });
    std::thread retirer([&] {
        arrive();
        for (long i = 0; i < iterations || !admitter_done.load(std::memory_order_acquire); ++i) {
            if (g.retire(v)) {
                // Only this thread registers /c/h, so it stays retired until the end of the turn:
                // looked at twice, once at once and once after the other thread had a chance.
                ++retired_states;
                held += g.own_subs(v) != 0 || published(v) != 0;
                std::this_thread::yield();
                held += g.own_subs(v) != 0 || published(v) != 0;
            }
            (void)g.try_register_vertex(path_t("/c/h"), role_t::STORED_VALUE);
            (void)g.try_register_vertex(path_t("/c/h/k"), role_t::STORED_VALUE);
            std::this_thread::yield();  // a registered window, so admissions land too
        }
    });
    admitter.join();
    retirer.join();
    g.collect();
    std::printf("    %ld admissions landed, %ld retired states looked at\n", landed,
                retired_states);
    check(landed > 0 && retired_states > 0, "both threads made progress");
    check(held == 0, "every retired state held no edge and a zero count");
    check(agree(g, v, kid, 0), "afterwards the count, the edges and the descendant all read 0");
}

}  // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : kIterations;
    test_count_follows_edges();
    test_retired_vertex_takes_no_edge();
    test_decision_does_not_cross_a_retire();
    test_concurrent_lifecycle(iterations);
    return tr::testing::summary("edge_count_lifecycle");
}
