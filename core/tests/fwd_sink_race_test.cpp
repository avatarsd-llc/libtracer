/**
 * @file
 * @brief #914 — a router sink's `{fn, ctx}` pair is published as a UNIT, so no receive
 *        thread can hand a newly installed `fn` the previous sink's `ctx`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `fwd_router_t` held its five observer/terminus sinks as ten plain non-atomic members.
 * Each setter stored the two halves separately with no lock — `raw_cb_ = fn; raw_ctx_ =
 * ctx;` — while the frame path did check-then-call on a transport receive thread. Besides
 * being a data race by the C++ memory model, that has a directly observable failure: a
 * reader that loads the pointer AFTER the `fn` store and the context BEFORE the `ctx`
 * store calls the NEW sink with the OLD sink's context. Every sink here takes its context
 * as `void*` and casts it, so a torn pair is a wrong-object call — the bug class the
 * documented runtime clear ("passing nullptr clears a sink") invites by design.
 *
 * The detector is a self-identifying context: sink A is only ever installed with probe A,
 * sink B only with probe B, and each sink checks the tag of the probe it was handed. One
 * thread flips the pair A→B→A… while another pumps frames through the router, so a torn
 * publish shows up as a tag mismatch — a functional assertion on every build, not only a
 * TSan vehicle (the race itself is also reported directly under `-fsanitize=thread`).
 *
 * Two of the five sinks are driven, chosen because they sit on different frame paths: the
 * raw-frame observer (every inbound frame, before any parse) and the stale-label observer
 * (the RFC-0004 §E.1 self-heal, reached only by an unknown-label COMPACT). Each scenario
 * also carries a positive control, so a "fix" that quietly stopped dispatching would fail
 * rather than pass by silence.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

/** @brief A sink context that knows which sink it belongs to. */
struct probe_t {
    char tag;                  /**< @brief 'A' or 'B' — whose context this is. */
    std::atomic<long> hits{0}; /**< @brief Dispatches that reached the right sink. */
};

/** @brief Torn `{fn, ctx}` pairs observed across all sinks — must stay zero. */
std::atomic<long> g_torn{0};

/**
 * @brief Score one dispatch: the sink was handed a context, and it must be ITS context.
 * @param ctx      The `void*` the router passed back.
 * @param expected The tag of the probe this sink is only ever installed with.
 */
void score(void* ctx, char expected) {
    auto* const p = static_cast<probe_t*>(ctx);
    if (p == nullptr || p->tag != expected) {
        g_torn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p->hits.fetch_add(1, std::memory_order_relaxed);
}

void raw_a(void* ctx, std::string_view, std::span<const std::byte>) { score(ctx, 'A'); }
void raw_b(void* ctx, std::string_view, std::span<const std::byte>) { score(ctx, 'B'); }
void stale_a(void* ctx, std::string_view, std::uint16_t) { score(ctx, 'A'); }
void stale_b(void* ctx, std::string_view, std::uint16_t) { score(ctx, 'B'); }

// --- wire builders (canonical bytes via the production emit helpers) ----------
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

using tr::testing::b_fwd;

/** @brief A span link that only counts — the frame path must keep working under the storm. */
class counting_link_t : public transport_t {
   public:
    void send(std::span<const std::byte>) override { ++sent_; }
    [[nodiscard]] std::size_t sent() const { return sent_; }

   private:
    std::size_t sent_ = 0;
};

constexpr int kIters = 200000;

/**
 * @brief How many dispatches must land for the torn-pair verdict to mean anything.
 *
 * Deliberately not "most frames". The flipper here publishes back-to-back for the whole
 * window — which is what makes the pre-fix tear reproducible — and a slot under a publish
 * reads as EMPTY by design, so a continuous setter storm legitimately swallows a large and
 * variable share of dispatches. Asserting a majority made this test flaky at ~12% on the
 * stale path. The number that matters is that tens of thousands of dispatches DID run and
 * every one of them was handed its own context; the exact share is printed, not asserted.
 */
constexpr long kFloor = kIters / 50;

/**
 * @brief The raw-frame observer: flipped from one thread while frames flow on another.
 */
void raw_sink_flip_race() {
    std::printf("raw-frame observer, flipped under a frame storm:\n");
    graph_t g;
    fwd_router_t router(g);
    counting_link_t cli;
    counting_link_t up;
    (void)router.add_child("cli", cli);
    (void)router.add_child("up", up);

    probe_t a{'A'};
    probe_t b{'B'};
    const std::vector<std::byte> frame =
        b_fwd(fwd_op_t::READ, b_path({"up", "sensor"}), b_path({"reply-ep"}));

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    // The flipper runs for as long as frames flow, NOT for a fixed count: a forward hop
    // costs far more than a `set`, so a counted flipper finishes in the first few percent
    // of the storm and leaves the rest of the pump running unopposed — which is how a
    // torn-pair detector passes vacuously.
    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!stop.load(std::memory_order_relaxed)) {
            router.on_raw(&raw_a, &a);
            router.on_raw(&raw_b, &b);
        }
    });
    std::thread pump([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) router.on_frame("cli", frame);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    pump.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d frames reached a sink)\n", hits, kIters);
    // Positive control: the storm must not have cost the router its day job, and the
    // observer must actually have observed — a sink that never fired proves nothing.
    check(up.sent() == static_cast<std::size_t>(kIters), "every frame still forwarded");
    check(hits > kFloor, "the raw observer fired on a large share of frames");
    check(g_torn.load() == 0, "no sink was handed the other sink's context");
}

/**
 * @brief The stale-label observer: a different frame path, the same publish discipline.
 */
void stale_sink_flip_race() {
    std::printf("stale-label observer, flipped under an unknown-label COMPACT storm:\n");
    graph_t g;
    fwd_router_t router(g);
    counting_link_t in;
    (void)router.add_child("in", in);

    probe_t a{'A'};
    probe_t b{'B'};
    const std::byte pl[2] = {std::byte{0xAA}, std::byte{0xBB}};
    std::vector<std::byte> value;
    tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, std::span<const std::byte>(pl, 2));
    // Label 7 was never advertised on "in": every one of these takes the §E.1 self-heal.
    const std::vector<std::byte> frame = tr::net::encode_compact(7, value);

    const long torn_before = g_torn.load();
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!stop.load(std::memory_order_relaxed)) {
            router.on_stale_label(&stale_a, &a);
            router.on_stale_label(&stale_b, &b);
        }
    });
    std::thread pump([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) router.on_frame("in", frame);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    pump.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d frames reached a sink)\n", hits, kIters);
    check(in.sent() == static_cast<std::size_t>(kIters), "every stale label still NACKed back");
    check(hits > kFloor, "the stale observer fired on a large share of frames");
    check(g_torn.load() == torn_before, "no sink was handed the other sink's context");
}

}  // namespace

int main() {
    std::printf("router sink publish coherence (#914):\n");
    raw_sink_flip_race();
    stale_sink_flip_race();
    return tr::testing::summary("fwd_sink_race");
}
