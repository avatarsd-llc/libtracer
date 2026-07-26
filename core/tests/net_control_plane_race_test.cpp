/**
 * @file
 * @brief ADR-0063 §3 — control-plane writers are serialized, and the forward reader is not.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0014 made connection create/remove a RUNTIME operation, and `make_connection` runs on
 * whichever transport's RECEIVE thread delivered the CREATE — so on an ordinary node with two
 * transports, two creates are genuinely concurrent. Nothing serialized them: `add_child`
 * performs a scan-then-append on the registry (two writers could be handed the SAME empty
 * slot, silently losing a child) and an `emplace_back` on the receiver deque (whose spine two
 * writers corrupt), while `add`'s REBIND path plainly wrote `multi_peer` under a plain read
 * from the forward descent.
 *
 * This test is built to FAIL under `-fsanitize=thread` without those fixes, and it is shaped
 * by what would NOT fail:
 *
 *   - **It churns create → remove → create of the SAME name.** Increment 1's append ordering
 *     is correct (the slot is filled before the `link` and `used` release-stores, and readers
 *     acquire-load both), so a test that only adds NEW names races nothing and passes. The
 *     rebind path — which RFC-0014 create/remove churn takes constantly — is the exposed one.
 *   - **Two writers work on DISJOINT name sets.** Same-name writers would merely contend; the
 *     registry corruption needs two appends of two different new names landing together.
 *   - **A reader runs the whole time**, driving the mount descent over the same table, so a
 *     writer-vs-reader race on the slot fields is live rather than theoretical.
 *
 * It is a race detector's test: with TSan it reports, without TSan it is a smoke test that the
 * churn does not crash or lose children. Both are worth running, so it is not TSan-gated.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A link that counts sends and never blocks — the churn must not be I/O-bound. */
struct sink_t : tr::net::transport_t {
    std::atomic<std::size_t> sends{0};
    void send(std::span<const std::byte>) override {
        sends.fetch_add(1, std::memory_order_relaxed);
    }
    void send(std::span<const std::span<const std::byte>>) override {
        sends.fetch_add(1, std::memory_order_relaxed);
    }
};

void emit_path(std::vector<std::byte>& out, std::span<const std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

/** @brief `FWD{ op=WRITE, dst, src, VALUE }` — the frame the forward descent walks. */
std::vector<std::byte> make_fwd(std::span<const std::string_view> dst) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    const std::string_view reply[] = {"reply"};
    emit_path(body, std::span<const std::string_view>(reply, 1));
    const std::byte payload[2] = {std::byte{0xAB}, std::byte{0xCD}};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 2));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief How many create/remove cycles each writer runs — bounded, so CI cannot hang. */
constexpr int kRounds = 4000;
/** @brief Distinct names per writer. Small, so the churn keeps REBINDING rather than growing. */
constexpr std::size_t kNamesPerWriter = 4;

}  // namespace

int main() {
    std::printf("control-plane create/remove churn against a live forward reader (ADR-0063)\n");

    graph_t g;
    fwd_router_t router(g);

    // The inbound link is registered once and never churned — the reader needs a stable door.
    sink_t inbound;
    router.add_child("net/in/up", inbound);

    // Every sink outlives the router's use of it: `add_child` stores the address, so a sink
    // destroyed while registered would be a use-after-free unrelated to what is under test.
    std::array<sink_t, 2 * kNamesPerWriter> sinks;

    const auto writer = [&](int w) {
        for (int r = 0; r < kRounds; ++r) {
            for (std::size_t i = 0; i < kNamesPerWriter; ++i) {
                std::string name = "net/";
                name += static_cast<char>('a' + w);
                name += "/c";
                name += static_cast<char>('0' + static_cast<char>(i));
                sink_t& s = sinks[static_cast<std::size_t>(w) * kNamesPerWriter + i];
                // create -> remove -> create of the SAME name: the second add takes the
                // REBIND path over the tombstone, which is the exposed one.
                router.add_child(name, s);
                (void)router.remove_child(name);
                router.add_child(name, s);
            }
        }
    };

    std::atomic<bool> stop{false};
    const auto reader = [&] {
        const std::string_view dst[] = {"net", "a", "c0", "leaf"};
        const std::vector<std::byte> frame = make_fwd(std::span<const std::string_view>(dst, 4));
        while (!stop.load(std::memory_order_relaxed)) {
            // Drives resolve_mount_segs over the churning table: the mount descent reads each
            // slot's name, `multi_peer` and `link` with no lock, by design (ADR-0038 §3).
            router.on_frame("net/in/up", frame);
        }
    };

    std::thread ra(reader);
    std::thread rb(reader);
    std::thread wa(writer, 0);
    std::thread wb(writer, 1);
    wa.join();
    wb.join();
    stop.store(true, std::memory_order_relaxed);
    ra.join();
    rb.join();

    // Every name each writer touched must still resolve. A LOST child is exactly what two
    // writers handed the same appended slot produces, and it is otherwise silent.
    int missing = 0;
    for (int w = 0; w < 2; ++w) {
        for (std::size_t i = 0; i < kNamesPerWriter; ++i) {
            std::string name = "net/";
            name += static_cast<char>('a' + w);
            name += "/c";
            name += static_cast<char>('0' + static_cast<char>(i));
            if (router.registry().by_name(name) == nullptr) ++missing;
        }
    }
    check(missing == 0, "every churned name still resolves — no child was lost to a shared slot");

    // And no name grew a SECOND slot. `add` is one-to-one by contract (#524's shadow-slot
    // use-after-free), so the live count is exactly the inbound link plus each writer's names
    // — a duplicate would inflate it even while every lookup above still succeeded.
    check(router.registry().live_size() == 1 + 2 * kNamesPerWriter,
          "and no name grew a second slot — the name→slot mapping stayed one-to-one");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
