/**
 * @file
 * @brief #684 — a rebind of a LIVE name must not touch the published mount encoding.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `child_registry_t::add`'s rebind path used to move-assign `mount_tlv` — freeing the
 * old heap buffer — while the forward path reads that same vector as a span with no
 * lock, on a transport receive thread. Re-registering a name that is already live,
 * concurrently with a frame in flight, was a use-after-free whose symptom is a
 * corrupted mount prefix on an EMITTED frame — a silent misroute at the next hop.
 *
 * The fix (ADR-0073's sibling ruling): the mount encoding is IMMUTABLE after publish —
 * it is a pure function of the slot's key, and a rebind matches by name, so the
 * replacement bytes were identical anyway. The rebind now updates only the fields a
 * reconnect can change (`multi_peer`, `link`).
 *
 * This test drives the exact two-thread shape from the issue: one thread loops
 * `add_child(same_name, link)` — what a reconnect does — while another routes frames
 * through `on_frame` for that inbound name, which reads the slot's mount span to grow
 * `src`. Under `-fsanitize=thread` the pre-fix code reports the race directly; the
 * test also asserts the data-plane outcome (every forward emitted, every emitted
 * frame byte-identical) so it is a functional regression test on every build, not
 * only a TSan vehicle.
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

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// --- wire builders (canonical bytes via the production emit helpers) ----------
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

using tr::testing::b_fwd;

/** @brief A span link recording every send — the downstream egress under observation. */
class fake_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());
    }
    std::vector<std::vector<std::byte>>& sent() { return sent_; }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

/**
 * @brief One thread rebinds the inbound name in a loop; another forwards frames whose
 *        `src` growth reads that name's published mount span.
 */
void rebind_race() {
    graph_t g;
    fwd_router_t router(g);
    fake_link_t cli;
    fake_link_t up;
    router.add_child("cli", cli);  // the inbound link a "reconnect" re-adds
    router.add_child("up", up);    // the dst-resolved forward child

    // dst=/up/sensor forwards to "up"; the hop strips "up" and PREPENDS the inbound
    // slot's mount run to src — the read side of the #684 race.
    const std::vector<std::byte> frame =
        b_fwd(fwd_op_t::READ, b_path({"up", "sensor"}), b_path({"reply-ep"}));

    constexpr int kIters = 4000;
    std::atomic<bool> go{false};

    std::thread rebinder([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) router.add_child("cli", cli);
    });
    std::thread forwarder([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) router.on_frame("cli", frame);
    });
    go.store(true, std::memory_order_release);
    rebinder.join();
    forwarder.join();

    // Positive control: an over-eager "fix" that broke rebinding or forwarding would
    // show up here — every frame must have been forwarded, none dropped.
    check(up.sent().size() == static_cast<std::size_t>(kIters),
          "every frame forwarded across the rebind storm");

    // The misroute detector: the emitted bytes are a pure function of the (constant)
    // input frame and the (immutable) mount run, so all emissions must be identical.
    // Pre-fix, a torn mount read corrupts the grown src on some emissions.
    bool identical = !up.sent().empty();
    for (const std::vector<std::byte>& s : up.sent())
        if (s != up.sent().front()) identical = false;
    check(identical, "every emitted frame byte-identical (no corrupted mount prefix)");

    // And the rebind kept doing its job: the slot still resolves to the live link.
    check(router.registry().by_name("cli") != nullptr, "the rebound name still resolves");
}

}  // namespace

int main() {
    std::printf("registry rebind vs forward (#684 — mount run immutable after publish):\n");
    rebind_race();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
