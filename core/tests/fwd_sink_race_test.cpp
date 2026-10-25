/**
 * @file
 * @brief #914 — a router sink's `{fn, ctx}` pair is published as a UNIT, so no receive
 *        thread can hand a newly installed `fn` the previous sink's `ctx`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `fwd_router_t` held its five observer/terminus sinks as ten plain non-atomic members.
 * Each setter stored the two halves separately with no lock — `reply_cb_ = fn; reply_ctx_ =
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
 * The reply-terminus sink is driven, with REPLY frames whose route ends at this node: it is
 * the one router sink left (the two frame observers were deleted, #2277; the stale-label
 * observer went with the label tables, #1951). The scenario carries a non-vacuity check, so a
 * "fix" that quietly stopped dispatching would fail rather than pass by silence.
 *
 * @par Why no SHARE of dispatches is asserted, only that some landed (#1380)
 * There used to be a `kFloor = kIters / 50` here and a "fired on a large share of frames"
 * check, and the share it named was never measured — it was a round fraction, and it landed
 * exactly on the observable: the aarch64 leg scored 3870 against a floor of 4000 and
 * reddened an unrelated PR. That is not a marginal bar, it is a bar placed at the value it
 * is judging. The share is not a property of the router either. The flipper publishes
 * back-to-back for the whole window — which is what makes the pre-fix tear reproducible —
 * and a slot under a publish reads as EMPTY by design, so what fraction of dispatches
 * survive is decided by the ratio between the cost of one `set` and the cost of the loop
 * branch between two of them. That ratio is a property of the MICROARCHITECTURE: on
 * x86-64's TSO the release fence inside `set` is free and the share is tens of percent; on
 * aarch64 it is a real `dmb ish` and the same code legitimately swallows fifty times more.
 * Asserting a majority made this test flaky at ~12 %; asserting 2 % moved the flake to the
 * other leg. So the verdict is the torn-pair invariant alone, plus a non-vacuity check that a
 * sink was reached at all. The share itself is PRINTED, which is
 * the number a human reads when a host looks degenerate.
 *
 * @par The `armed` latch
 * Non-vacuity gets a structural half as well: both threads wait on `go`, so nothing stopped
 * the pump from running its whole storm before the flipper's first publish and scoring a
 * legitimate zero. The flipper now installs one sink and announces it, and the pump does not
 * send its first frame until it sees that — so the storm always runs against an installed
 * sink, for a reason that has nothing to do with the scheduler.
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
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::reply_kind_t;
using tr::net::fwd_router_t;
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

void reply_a(void* ctx, const tr::view::rope_t&) { score(ctx, 'A'); }
void reply_b(void* ctx, const tr::view::rope_t&) { score(ctx, 'B'); }

// --- wire builders (canonical bytes via the production emit helpers) ----------
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

using tr::testing::b_fwd_reply;

/** @brief Frames each storm pumps through the router. */
constexpr int kIters = 200000;

/**
 * @brief The reply-terminus sink: flipped from one thread while replies flow on another.
 */
void reply_sink_flip_race() {
    std::printf("reply-terminus sink, flipped under a reply storm:\n");
    graph_t g;
    fwd_router_t router(g);

    probe_t a{'A'};
    probe_t b{'B'};
    // `reply-ep` names no child, so the route is spent here and the reply goes to the sink.
    const std::byte pv{0x2A};
    std::vector<std::byte> value;
    tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, std::span<const std::byte>(&pv, 1));
    const std::vector<std::byte> frame =
        b_fwd_reply(reply_kind_t::RESULT, b_path({"reply-ep"}), b_path({"sensor"}), value);

    std::atomic<bool> go{false};
    std::atomic<bool> armed{false};
    std::atomic<bool> stop{false};
    // The flipper runs for as long as frames flow, NOT for a fixed count: a forward hop
    // costs far more than a `set`, so a counted flipper finishes in the first few percent
    // of the storm and leaves the rest of the pump running unopposed — which is how a
    // torn-pair detector passes vacuously.
    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        // Arm with A installed and say so, so the storm never runs against an empty slot
        // for a reason that has nothing to do with the seam under test.
        router.on_reply(&reply_a, &a);
        armed.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            router.on_reply(&reply_a, &a);
            router.on_reply(&reply_b, &b);
        }
    });
    std::thread pump([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!armed.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) router.on_frame("cli", frame);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    pump.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d frames reached a sink)\n", hits, kIters);
    // The sink must actually have been reached — a sink that never fired proves nothing.
    // The SHARE is printed above, never asserted; see the file header for why.
    check(hits > 0, "the reply sink was reached during the storm (the torn check is not vacuous)");
    check(g_torn.load() == 0, "no sink was handed the other sink's context");
}

}  // namespace

int main() {
    std::printf("router sink publish coherence (#914):\n");
    reply_sink_flip_race();
    return tr::testing::summary("fwd_sink_race");
}
