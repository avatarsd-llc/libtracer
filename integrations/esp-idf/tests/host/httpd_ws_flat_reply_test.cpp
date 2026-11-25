/**
 * @file
 * @brief #2911: on a FLAT `httpd_ws_link_t`, a reply goes to the WebSocket session that sent
 *        the request.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The REAL chip translation unit (`httpd_ws_link.cpp`) on the host fake of `esp_http_server`,
 * with a real `graph_t` and `fwd_router_t` behind it. Two sessions on one link with
 * `peer_named = false`:
 *
 *   1. a READ from A is answered to A's socket, and B's socket receives nothing;
 *   2. a deferred AWAIT from A, answered by a write from the application's thread, goes to A
 *      alone;
 *   3. an AWAIT whose session left before the write is answered to nobody, and a new session
 *      in A's slot does not receive it either.
 *
 * Ablation: `httpd_ws_link_t::send_to` calling `send` (the broadcast) reddens 1 and 2;
 * dropping its handle compare reddens 3.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "fake_httpd.hpp"
#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::httpd_ws_link_t;
using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::make_value;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief Drain the fake's control queue: the httpd task's own loop. */
void run_server() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief Bytes @p fd received since the last @ref clear_sends: a frame written in the call
 *         (recorded as it is sent) or one written by a queued send (recorded on the wire). */
std::size_t received(int fd) {
    std::size_t n = fake_httpd::instance().wire(fd).size();
    for (const fake_httpd::server_t::sent_frame_t& f : fake_httpd::instance().sent_frames())
        if (f.fd == fd) n += f.payload.size();
    return n;
}

/** @brief Forget what every session in @p fds received so far. */
void clear_sends(std::initializer_list<int> fds) {
    fake_httpd::instance().clear_sent_frames();
    for (const int fd : fds) fake_httpd::instance().clear_wire(fd);
}

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> raw(4);
    tr::detail::store_le<std::uint32_t>(raw, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief `FWD{AWAIT, dst=/sink, src=/cli, await_timeout=10 s}`. */
std::vector<std::byte> b_await() {
    std::vector<std::byte> raw(8);
    tr::detail::store_le<std::uint64_t>(raw, 10'000'000'000ULL);
    std::vector<std::byte> timeout;
    tr::wire::emit_tlv(timeout, type_t::VALUE, opt_t{}, raw);
    return b_fwd(fwd_op_t::AWAIT, b_path({"sink"}), b_path({"cli"}), {}, timeout);
}

void replies_go_to_the_asking_session() {
    std::printf("#2911 flat httpd WS link: each reply goes to the session that asked:\n");
    fake_httpd::instance().close_all();
    run_server();
    fake_httpd::instance().clear_sent_frames();

    graph_t g;
    const auto sink = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    (void)g.write(sink, make_value(b_value_u32(1)));
    fwd_router_t router(g);
    auto link = tr::mem::make_poly<httpd_ws_link_t>(
        tr::mem::net_source(), handle(), "/ws", tr::net::httpd_ws_config_t{.peer_named = false});
    check(router.attach_link("cli", *link), "the link is a routed child");

    constexpr int kA = 930;
    constexpr int kB = 931;
    // A session is claimed by its first frame, so each one sends a READ before the cases.
    const std::vector<std::byte> read = b_fwd(fwd_op_t::READ, b_path({"sink"}), b_path({"cli"}));
    for (const int fd : {kA, kB}) {
        (void)fake_httpd::instance().open_session(fd);
        (void)fake_httpd::instance().deliver_frame(fd, read);
    }
    run_server();
    check(received(kB) != 0, "B is claimed and answered");
    clear_sends({kA, kB});

    // 1. READ, answered in the handler frame.
    (void)fake_httpd::instance().deliver_frame(kA, read);
    run_server();
    check(received(kA) != 0, "A's READ is answered to A");
    check(received(kB) == 0, "and B receives nothing");

    // 2. A deferred AWAIT, answered by a write on this (the application's) thread.
    clear_sends({kA, kB});
    (void)fake_httpd::instance().deliver_frame(kA, b_await());
    run_server();
    check(router.pending_awaits() == 1, "A's AWAIT parks");
    (void)g.write(sink, make_value(b_value_u32(2)));
    run_server();
    check(received(kA) != 0, "the write answers A's AWAIT to A");
    check(received(kB) == 0, "and B receives nothing");

    // 3. A leaves with an AWAIT parked; a new session takes its slot.
    clear_sends({kA, kB});
    (void)fake_httpd::instance().deliver_frame(kA, b_await());
    run_server();
    check(router.pending_awaits() == 1, "A's second AWAIT parks");
    fake_httpd::instance().close_session(kA);
    run_server();
    constexpr int kC = 932;
    (void)fake_httpd::instance().open_session(kC);
    (void)fake_httpd::instance().deliver_frame(kC, read);
    run_server();
    clear_sends({kB, kC});
    (void)g.write(sink, make_value(b_value_u32(3)));
    run_server();
    check(received(kB) == 0, "after A has left, B receives nothing");
    check(received(kC) == 0, "and neither does the session in A's slot");

    fake_httpd::instance().close_all();
    run_server();
}

}  // namespace

int main() {
    replies_go_to_the_asking_session();
    if (g_failures == 0) {
        std::printf("httpd_ws_flat_reply: OK\n");
        return 0;
    }
    std::printf("httpd_ws_flat_reply: %d FAILURES\n", g_failures);
    return 1;
}
