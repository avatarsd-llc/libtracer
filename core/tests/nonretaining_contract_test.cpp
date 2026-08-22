/**
 * @file
 * @brief RFC-0008 Amendment 2 (#1506) — the non-retaining-vertex contract, five vectors.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A `HANDLER` vertex retains nothing: `graph_t::store_value` hands the value to `on_write` and
 * publishes no last-known-value. Two consequences the amendment ratifies, each pinned here with
 * a POSITIVE CONTROL so a vector that passes for the wrong reason is visible:
 *
 * 1. `await` is the readiness form of a data READ (RFC-0008 §A), so after a wake it serves the
 *    value through the SAME role dispatch `read` runs — vectors 1-3.
 * 2. `assign` / `propagate` are the accumulate-then-flush pair, and the flush half takes no
 *    value argument, so at a vertex that retains nothing the pair had nothing to move and swept
 *    silence. They now refuse at the verb — vectors 4-5.
 *
 * Every vector fails with the two production hunks in `core/src/graph.cpp` reverted: vectors
 * 1-3 by `await` answering NOT_FOUND after the awaited write landed, vectors 4-5 by the refusal
 * never arriving.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::emission_mode_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief The single byte a one-byte rope carries (0 when the rope is not that shape). */
[[nodiscard]] std::uint8_t only_byte(const rope_t& r) {
    if (r.link_count() != 1 || r.links()[0].bytes().size() != 1) return 0;
    return std::to_integer<std::uint8_t>(r.links()[0].bytes()[0]);
}

/**
 * @brief Await @p v while a writer thread re-arms the write until the await is out.
 *
 * `await` is EDGE-triggered — it snapshots `write_seq_` on entry — so a single write racing the
 * call is legitimately unobservable (#1418). Re-arming from the writer removes the race without
 * guessing at a sleep, under a deadline that exceeds the await timeout so a broken wake path
 * fails rather than hangs. Every re-arm writes the SAME byte, so the value assertion is exact.
 */
[[nodiscard]] tr::graph::result_t<tr::graph::value_ref_t> await_while_writing(graph_t& g,
                                                                              vertex_handle_t v,
                                                                              std::uint8_t byte) {
    std::atomic<bool> done{false};
    std::thread writer([&] {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)g.write(v, make_value({byte}));
            std::this_thread::sleep_for(2ms);
        }
    });
    auto got = g.await(v, 5s);
    done.store(true, std::memory_order_release);
    writer.join();
    return got;
}

/**
 * @brief Vector 1 — a HANDLER vertex: WRITE then AWAIT answers the on_read-composed VALUE.
 *
 * Pre-amendment this was `NOT_FOUND` *after the awaited write arrived*: the write reached
 * `on_write`, bumped the sequence and woke the waiter, and the waiter then asked the LKV slot a
 * handler never fills. The positive control is the same vertex's `read`, which has always
 * composed from the seam — the two doors must agree at the same instant.
 */
void test_await_at_a_handler_serves_the_read_contract() {
    std::printf("vector 1 — AWAIT at a HANDLER composes from on_read:\n");
    graph_t g;
    auto last = std::make_shared<std::uint8_t>(0);
    handlers_t h;
    h.on_write = [last](const rope_t& value,
                        const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        *last = only_byte(value);
        return {};
    };
    h.on_read = [last]() -> tr::graph::result_t<rope_t> { return rope_t{make_value({*last})}; };
    vertex_handle_t v = g.register_vertex(path_t("/h/seam"), role_t::HANDLER, std::move(h));

    // POSITIVE CONTROL: the seam answers `read`. If this fails the vector below proves nothing.
    check(g.write(v, make_value({0x2A})).has_value(), "control — the handler accepts a write");
    const auto rd = g.read(v);
    check(rd.has_value() && only_byte(**rd) == 0x2A,
          "control — `read` at the handler composes 0x2A from on_read");

    const auto got = await_while_writing(g, v, 0x5B);
    check(got.has_value(), "AWAIT at a handler returns a VALUE, not an error");
    check(got.has_value() && only_byte(**got) == 0x5B,
          "and the value is the one on_read composes — the same answer `read` gives");
}

/**
 * @brief Vector 2 — a HANDLER exposing no `on_read`: AWAIT still answers NOT_FOUND.
 *
 * The degradation belongs to the READ contract, not to await: a vertex that answers no read
 * answers no awaited read either. This is what keeps Amendment 2 from being "await always
 * succeeds at a handler".
 */
void test_await_at_a_seamless_handler_is_still_not_found() {
    std::printf("vector 2 — AWAIT at a HANDLER with no on_read stays NOT_FOUND:\n");
    graph_t g;
    handlers_t h;
    h.on_write = [](const rope_t&, const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        return {};
    };
    vertex_handle_t v = g.register_vertex(path_t("/h/bare"), role_t::HANDLER, std::move(h));

    // POSITIVE CONTROL: `read` refuses this vertex for exactly the same reason.
    const auto rd = g.read(v);
    check(!rd && rd.error() == status_t::NOT_FOUND,
          "control — `read` at a seamless handler is NOT_FOUND");

    const auto got = await_while_writing(g, v, 0x11);
    check(!got && got.error() == status_t::NOT_FOUND,
          "AWAIT answers NOT_FOUND too — the read contract's degradation, not await's");
    check(!got || got.error() != status_t::TIMEOUT,
          "and it is NOT a timeout: the write did wake the waiter");
}

/**
 * @brief Vector 3 — REGRESSION PIN: a retaining vertex's AWAIT is unchanged.
 *
 * The stored arm keeps its `read_stored()` fast path; no handler dispatch was added to it. A
 * branch vertex is pinned too: `await` watches its OWN write sequence (RFC-0008 §A), so it
 * hands back its own last-known-value, never the composed subtree fold `read` would serve.
 */
void test_await_at_a_retaining_vertex_is_unchanged() {
    std::printf("vector 3 — AWAIT at a retaining vertex is byte-identical:\n");
    graph_t g;
    vertex_handle_t branch = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    vertex_handle_t leaf = g.register_vertex(path_t("/s/leaf"), role_t::STORED_VALUE);

    const auto got = await_while_writing(g, leaf, 0x77);
    check(got.has_value() && only_byte(**got) == 0x77, "AWAIT returns the published LKV");
    const auto rd = g.read(leaf);
    check(rd.has_value() && only_byte(**rd) == 0x77, "control — `read` returns the same bytes");

    // The branch pin: /s has a registered child, so `read` composes a POINT fold while `await`
    // must still hand back /s's own value.
    const auto b = await_while_writing(g, branch, 0x33);
    check(b.has_value() && only_byte(**b) == 0x33,
          "AWAIT at a BRANCH is its own LKV, not the composed subtree fold");
    const auto folded = g.read(branch);
    check(folded.has_value() && (*folded)->link_count() >= 1,
          "control — `read` at the same branch composes a (larger) fold");
    check(folded.has_value() && only_byte(**folded) != 0x33,
          "and the two answers differ, so the branch fork really was not mirrored");
}

/**
 * @brief Vector 4 — `assign` at a non-retaining vertex refuses BY VALUE and queues nothing.
 *
 * The refusal is at the verb, so there is nothing downstream to count: `on_write` is never
 * entered, no pending mark is inserted, and the covering sweep from the retaining parent
 * delivers nothing. The positive control is the sibling `STORED_VALUE` vertex under the same
 * parent riding the same sweep.
 */
void test_assign_at_a_non_retaining_vertex_refuses() {
    std::printf("vector 4 — assign at a HANDLER refuses by value:\n");
    graph_t g;
    auto writes = std::make_shared<int>(0);
    handlers_t h;
    h.on_write = [writes](const rope_t&,
                          const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        ++*writes;
        return {};
    };
    vertex_handle_t root = g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    vertex_handle_t hv = g.register_vertex(path_t("/r/h"), role_t::HANDLER, std::move(h));
    vertex_handle_t sv = g.register_vertex(path_t("/r/s"), role_t::STORED_VALUE);

    auto at_h = std::make_shared<int>(0);
    auto at_s = std::make_shared<int>(0);
    auto on_h = [at_h](const rope_t&) { ++*at_h; };
    auto on_s = [at_s](const rope_t&) { ++*at_s; };
    check(g.subscribe(path_t("/r/h"), on_h).has_value(), "subscribe to the handler vertex");
    check(g.subscribe(path_t("/r/s"), on_s).has_value(), "subscribe to the stored sibling");

    const auto refused = g.assign(hv, make_value({0x01}));
    check(!refused && refused.error() == status_t::SCHEMA_NOT_FOUND,
          "assign at a handler answers SCHEMA_NOT_FOUND — the contract-mismatch status");
    check(refused.has_value() || refused.error() != status_t::BACKPRESSURE,
          "and NOT backpressure: nothing is under pressure and a retry never succeeds");
    check(*writes == 0, "the refusal is BEFORE the seam — on_write was never entered");

    // POSITIVE CONTROL: the retaining sibling accepts the same assign and rides the sweep.
    check(g.assign(sv, make_value({0x02})).has_value(), "control — the stored sibling assigns");
    check(*at_s == 0 && *at_h == 0, "control — assign is the state half; it delivers nothing");
    check(g.propagate(root).has_value(), "control — the covering sweep from /r succeeds");
    check(*at_s == 1, "control — the sweep delivers the sibling exactly once");
    check(*at_h == 0, "nothing was queued at the handler, so the sweep delivers it nothing");
}

/**
 * @brief Vector 5 — `propagate` at a non-retaining vertex refuses BY VALUE, in both modes.
 *
 * The other half of the pair, same status and same reason: the sweep root's own delivery reads
 * the last-known-value, so a root that retains nothing was always going to deliver silence.
 * Only the ROOT is judged — the control sweeps the same handler from its retaining parent and
 * is untouched.
 */
void test_propagate_at_a_non_retaining_vertex_refuses() {
    std::printf("vector 5 — propagate at a HANDLER refuses by value:\n");
    graph_t g;
    handlers_t h;
    h.on_read = []() -> tr::graph::result_t<rope_t> { return rope_t{make_value({0x09})}; };
    vertex_handle_t root = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    vertex_handle_t hv = g.register_vertex(path_t("/p/h"), role_t::HANDLER, std::move(h));

    auto at_h = std::make_shared<int>(0);
    auto at_p = std::make_shared<int>(0);
    auto on_h = [at_h](const rope_t&) { ++*at_h; };
    auto on_p = [at_p](const rope_t&) { ++*at_p; };
    check(g.subscribe(path_t("/p/h"), on_h).has_value(), "subscribe to the handler vertex");
    check(g.subscribe(path_t("/p"), on_p).has_value(), "subscribe to the retaining root");

    const auto refused = g.propagate(hv);
    check(!refused && refused.error() == status_t::SCHEMA_NOT_FOUND,
          "propagate rooted at a handler answers SCHEMA_NOT_FOUND");
    const auto refused_fold = g.propagate(hv, emission_mode_t::FOLD);
    check(!refused_fold && refused_fold.error() == status_t::SCHEMA_NOT_FOUND,
          "and FOLD answers alike — the refusal is the verb's, not the emission mode's");
    check(*at_h == 0, "the refused sweeps delivered nothing");

    // POSITIVE CONTROL: only the ROOT is judged. A sweep rooted at the retaining parent runs
    // exactly as before, walking past the non-retaining child without refusing.
    check(g.write(root, make_value({0x42})).has_value(), "control — write the retaining root");
    check(*at_p == 1, "control — the eager write delivered the root once");
    check(g.propagate(root).has_value(),
          "control — a sweep rooted at the retaining parent still succeeds");
    check(*at_p == 2, "control — and delivers the root's own value again");
    check(*at_h == 0, "the non-retaining child rides no sweep — it carries no mark");
}

}  // namespace

int main() {
    test_await_at_a_handler_serves_the_read_contract();
    test_await_at_a_seamless_handler_is_still_not_found();
    test_await_at_a_retaining_vertex_is_unchanged();
    test_assign_at_a_non_retaining_vertex_refuses();
    test_propagate_at_a_non_retaining_vertex_refuses();
    return tr::testing::summary("nonretaining_contract");
}
