/**
 * @file
 * @brief #1880 — every runtime growth site of `httpd_ws_link_t` draws from its injected
 *        stores, and a refusal there sheds ONE peer or ONE frame, counted, and leaves the
 *        link serving.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the other httpd suites: the REAL chip translation unit compiled
 * against the host fake of `esp_http_server` (fake_httpd.hpp). The instrument is a
 * @ref gated_source_t injected as `memory.state` (connection state) or `memory.io`
 * (egress), refused at the moment a case reaches the growth site under test:
 *   1. the session table — a new peer is refused like one past `max_peers`, counted on
 *      `peers_refused`, and the peers already admitted keep being served;
 *   2. the reassembly buffer — the fragmented message is dropped and charged to its
 *      session, which stays open;
 *   3. a receive buffer past the RX scratch — the frame is dropped as `rx_dropped_alloc`;
 *   4. the peer resolution — `peer_link` answers "no such peer" instead of a handle;
 *   5. a TX payload past the inline capacity — the frame is an enqueue drop.
 * Each case then lifts the refusal and shows the same site serving again, so a refusal is
 * never a latched failure. A sixth case checks the stores balance: everything the link drew
 * from them is returned by its destructor.
 */

#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "fake_httpd.hpp"
#include "gated_source.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using tr::net::httpd_ws_link_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief A minimal frame body — enough to claim a peer. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief Admit @p fd and deliver one whole frame on it; the handler's verdict. */
esp_err_t claim(int fd) {
    fake_httpd::instance().open_session(fd);
    return fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief Open peers the link enumerates. */
std::size_t open_peers(const httpd_ws_link_t& link) {
    std::size_t n = 0;
    link.enumerate_peers([&n](std::string_view) { ++n; });
    return n;
}

/** @brief Inbound drops charged to every open session. */
std::uint32_t session_rx_drops(const httpd_ws_link_t& link) {
    std::uint32_t n = 0;
    link.enumerate_peer_stats([&n](const httpd_ws_link_t::peer_stats_t& p) { n += p.c.rx_drops; });
    return n;
}

/** @brief A peer-named link adopting the fake, with @p state and @p io as its stores. */
tr::mem::poly_ptr_t<httpd_ws_link_t> make_link(gated_source_t* state, gated_source_t* io,
                                               std::size_t rx_scratch = 0,
                                               std::size_t tx_inline = 0) {
    return tr::mem::make_poly<httpd_ws_link_t>(
        tr::mem::net_source(), handle(), "/ws",
        tr::net::httpd_ws_config_t{.peer_named = true,
                                   .rx_scratch_bytes = rx_scratch,
                                   .tx_inline_bytes = tx_inline,
                                   .memory = {.rx = nullptr, .io = io, .state = state}});
}

/** @brief Retire the link and the fake's sessions between cases. */
void reset(tr::mem::poly_ptr_t<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().close_all();
    drain();
}

void test_session_table_refusal_sheds_the_new_peer() {
    std::printf("#1880 a refused session draw refuses the NEW peer, counted:\n");
    gated_source_t state;
    auto link = make_link(&state, nullptr);
    check(claim(800) == ESP_OK, "a first peer is admitted");
    const auto before = link->stats();

    state.refusing = true;
    check(claim(801) == ESP_FAIL, "a peer whose session cannot be drawn is refused cleanly");
    state.refusing = false;
    check(state.refused > 0, "the refusal came from the injected state store");
    check(link->stats().peers_refused == before.peers_refused + 1,
          "it is counted where a peer past max_peers is");
    check(open_peers(*link) == 1, "the peer already admitted is still served");

    check(claim(802) == ESP_OK, "once the store serves again, the next peer is admitted");
    check(open_peers(*link) == 2, "and enumerated beside the first");
    reset(link);
}

void test_reassembly_refusal_drops_the_message() {
    std::printf("#1880 a refused reassembly draw drops the message, keeps the session:\n");
    gated_source_t state;
    auto link = make_link(&state, nullptr);
    check(claim(810) == ESP_OK, "the peer is admitted");
    const std::uint32_t drops_before = session_rx_drops(*link);

    state.refusing = true;
    const esp_err_t first = fake_httpd::instance().deliver_frame(810, kBody, /*final=*/false);
    state.refusing = false;
    check(first == ESP_OK, "the first fragment is drained, not a reason to close the peer");
    check(session_rx_drops(*link) == drops_before + 1, "the lost message is charged to it");
    check(open_peers(*link) == 1, "and the session stays open");

    check(fake_httpd::instance().deliver_frame(810, kBody, /*final=*/false) == ESP_OK &&
              fake_httpd::instance().deliver_frame(810, kBody, /*final=*/true,
                                                   HTTPD_WS_TYPE_CONTINUE) == ESP_OK,
          "a fragmented message reassembles once the store serves again");
    check(session_rx_drops(*link) == drops_before + 1, "with no further drop");
    reset(link);
}

void test_receive_buffer_refusal_is_rx_dropped_alloc() {
    std::printf("#1880 a refused receive buffer past the scratch is rx_dropped_alloc:\n");
    gated_source_t state;
    auto link = make_link(&state, nullptr, /*rx_scratch=*/64);
    check(claim(820) == ESP_OK, "the peer is admitted");
    check(link->rx_scratch_bytes() == 64, "the scratch is the size asked for");
    const std::vector<std::byte> big(128, std::byte{0x5A});

    state.refusing = true;
    const esp_err_t err = fake_httpd::instance().deliver_frame(820, big);
    state.refusing = false;
    check(err == ESP_FAIL, "the frame cannot be drained, so the session is given up");
    check(link->stats().rx_dropped_alloc == 1, "counted as rx_dropped_alloc");

    check(claim(821) == ESP_OK && fake_httpd::instance().deliver_frame(821, big) == ESP_OK,
          "with the store serving, the same frame size is read on another peer");
    check(link->stats().rx_dropped_alloc == 1, "and nothing further is dropped");
    reset(link);
}

void test_resolution_refusal_is_no_such_peer() {
    std::printf("#1880 a refused resolution draw is an honest \"no such peer\":\n");
    gated_source_t state;
    auto link = make_link(&state, nullptr);
    check(claim(830) == ESP_OK, "the peer is admitted");

    state.refusing = true;
    tr::net::transport_t* const refused = link->peer_link("p0");
    state.refusing = false;
    check(refused == nullptr, "no handle is handed out when none can be drawn");
    check(open_peers(*link) == 1, "and the peer itself is untouched");

    tr::net::transport_t* const to = link->peer_link("p0");
    check(to != nullptr, "the next resolve draws a handle");
    to->send(std::span<const std::byte>(kBody));
    drain();
    check(link->stats().enqueue_drops == 0, "and it carries a frame");
    reset(link);
}

void test_tx_payload_refusal_is_an_enqueue_drop() {
    std::printf("#1880 a refused TX payload past the inline capacity is an enqueue drop:\n");
    gated_source_t io;
    auto link = make_link(nullptr, &io, /*rx_scratch=*/0, /*tx_inline=*/32);
    check(claim(840) == ESP_OK, "the peer is admitted");
    tr::net::transport_t* const to = link->peer_link("p0");
    check(to != nullptr, "the directed endpoint resolved");
    const std::vector<std::byte> big(256, std::byte{0x77});
    const auto before = link->stats();

    io.refusing = true;
    to->send(std::span<const std::byte>(big));
    io.refusing = false;
    check(io.refused > 0, "the refusal came from the injected egress store");
    check(link->stats().enqueue_drops == before.enqueue_drops + 1, "the frame is counted");

    to->send(std::span<const std::byte>(big));
    drain();
    check(link->stats().enqueue_drops == before.enqueue_drops + 1,
          "with the store serving, the same frame is sent");
    reset(link);
}

void test_the_stores_balance_at_teardown() {
    std::printf("#1880 everything drawn from the stores is returned by the destructor:\n");
    gated_source_t state;
    gated_source_t io;
    {
        auto link = tr::mem::make_poly<httpd_ws_link_t>(
            tr::mem::net_source(), std::uint16_t{0},
            tr::net::httpd_ws_config_t{.peer_named = true,
                                       .memory = {.rx = nullptr, .io = &io, .state = &state}});
        check(link->ok(), "an owning link came up on the fake");
        check(state.live > 0 && io.live > 0, "it drew its state and its TX pool from them");
        // Grow every connection-state site once: a session, a resolution handle and a
        // reassembly buffer left mid-message.
        check(claim(850) == ESP_OK, "a peer is admitted");
        check(link->peer_link("p0") != nullptr, "and resolved");
        check(fake_httpd::instance().deliver_frame(850, kBody, /*final=*/false) == ESP_OK,
              "and left mid-reassembly");
    }
    fake_httpd::instance().close_all();
    drain();
    check(state.live == 0, "every connection-state block came back");
    check(io.live == 0, "every egress block came back");
}

}  // namespace

int main() {
    std::printf("httpd_ws_link allocation-seam soft-fail host suite (#1880):\n");
    test_session_table_refusal_sheds_the_new_peer();
    test_reassembly_refusal_drops_the_message();
    test_receive_buffer_refusal_is_rx_dropped_alloc();
    test_resolution_refusal_is_no_such_peer();
    test_tx_payload_refusal_is_an_enqueue_drop();
    test_the_stores_balance_at_teardown();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
