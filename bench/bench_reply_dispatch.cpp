/**
 * @file
 * @brief The terminus REPLY's dispatch onto its link: a point-to-point link and a link that
 *        names the delivering session (#2911).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A READ arrives on a link and is answered over the same link. Since #2911 the answer leaves
 * through `transport_t::send_to(inbound_reply_to(), iov)`; before, through `send(iov)`. A
 * single-session link keeps the default `send_to`, which calls `send`, so the change costs it
 * two virtual calls per reply and nothing else. This bench prices exactly that, end to end
 * through the router's receive path (`rx_.deliver_borrowed` on the bound receiver):
 *
 * - `reply-p2p` — a link with no per-peer identity (`inbound_peer()` default);
 * - `reply-flat-handle` — a link whose `inbound_peer()` names the delivering session, the
 *   shape a FLAT stream listener has. It keeps the default `send_to`, so the row isolates the
 *   router's side; a real listener's own `send_to` adds its slot lookup.
 *
 * The same file builds on a tree before #2911 (where both rows reply through `send`), which is
 * the A/B. Batch-amortized; each row reports the BEST of rounds (contamination is one-sided).
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief A stub link: counts replies, and feeds a frame into the router's bound receiver. */
struct stub_link_t : tr::net::transport_t {
    tr::net::peer_handle_t peer{};
    std::size_t sends = 0;
    void send(std::span<const std::byte>) override { ++sends; }
    void send(std::span<const std::span<const std::byte>>) override { ++sends; }
    [[nodiscard]] tr::net::peer_handle_t inbound_peer() const noexcept override { return peer; }
    void feed(std::span<const std::byte> frame) const { rx_.deliver_borrowed(frame); }
};

/** @brief One PATH TLV of @p segs. */
void emit_path(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs)
        tr::wire::emit_tlv(body, type_t::NAME, opt_t{},
                           std::span(reinterpret_cast<const std::byte*>(s.data()), s.size()));
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

/** @brief Nanoseconds per reply on @p link, best of @p rounds batches of @p batch. */
double run_row(stub_link_t& link, std::span<const std::byte> frame, std::size_t batch,
               std::size_t rounds) {
    double best = 1e30;
    for (std::size_t r = 0; r < rounds; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < batch; ++i) link.feed(frame);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() /
                                  static_cast<double>(batch));
    }
    return best;
}

}  // namespace

int main() {
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router(graph);
    const auto v = graph.register_vertex(*tr::graph::path_t::parse("/sensor/temp"),
                                         tr::graph::role_t::STORED_VALUE);
    const std::byte payload[4] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};
    std::vector<std::byte> stored;
    tr::wire::emit_tlv(stored, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 4));
    (void)graph.write(v, tr::view::over_bytes(stored).value_or(tr::view::view_t{}));

    std::vector<std::byte> body;
    const std::byte rop{static_cast<std::uint8_t>(tr::graph::fwd_op_t::READ)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&rop, 1));
    emit_path(body, {"sensor", "temp"});
    emit_path(body, {"reply"});
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);

    stub_link_t p2p;
    stub_link_t flat;
    flat.peer = tr::net::peer_handle_t{tr::wire::pair_t{.index = 3, .generation = 7}};
    (void)router.attach_link("p2p", p2p);
    (void)router.attach_link("flat", flat);

    constexpr std::size_t kBatch = 20000;
    constexpr std::size_t kRounds = 15;
    for (stub_link_t* l : {&p2p, &flat}) {
        const std::size_t before = l->sends;
        l->feed(frame);
        if (l->sends != before + 1) {
            std::printf("FAIL: the READ was not answered on its link — fixture broken\n");
            return 2;
        }
    }
    // Interleaved: each round of each row alternates, so drift lands on both.
    double best_p2p = 1e30;
    double best_flat = 1e30;
    for (std::size_t r = 0; r < kRounds; ++r) {
        best_p2p = std::min(best_p2p, run_row(p2p, frame, kBatch, 1));
        best_flat = std::min(best_flat, run_row(flat, frame, kBatch, 1));
    }
    std::printf("RESULT reply-p2p ns=%.1f\n", best_p2p);
    std::printf("RESULT reply-flat-handle ns=%.1f\n", best_flat);
    return 0;
}
