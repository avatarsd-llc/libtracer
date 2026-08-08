/**
 * @file
 * @brief #955 — what the httpd WS link owes an ADOPTED server, and what it does about it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 / #835 / #954 / #957 suites: the REAL chip translation
 * unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) is compiled against the host
 * fake of `esp_http_server` (fake_httpd.hpp). What this suite adds to the fake are the two
 * server facts the adopting constructor cannot reach — the `httpd_config_t` a
 * `httpd_start` was given, and each session's LRU counter.
 *
 * The defect: the port-binding constructor establishes three server-level invariants
 * (`stack_size`, `lru_purge_enable = false`, a socket budget sized to `max_peers`) in the
 * one `httpd_config_t` it writes. The adopting constructor takes an ALREADY-STARTED
 * server, so it writes no config, and `esp_http_server` exposes no reader for a running
 * server's config either — it can neither apply those three nor check them. Two
 * consequences are addressed here:
 *
 *   1. `kHttpdTaskStack` was a constant in the .cpp's anonymous namespace, so the figure
 *      an embedder must configure was not readable from anywhere in the API. It is now
 *      `httpd_ws_link_t::kRequiredHttpdStack`, and the link samples the task's free-stack
 *      high-water mark at each session claim so a stack below it is NAMED once rather than
 *      arriving as an unexplained stack-protection reboot.
 *   2. IDF advances a session's LRU counter from inbound request processing only
 *      (`httpd_sess_process`), so on a purging host a graph peer that subscribes and
 *      thereafter only RECEIVES ages toward the lowest counter — it becomes the
 *      preferential victim of `httpd_accept_conn`'s purge no matter how much this link is
 *      pushing at it. In adopted mode the link now refreshes the counter after each
 *      successful send.
 *
 * What this suite deliberately does NOT claim:
 *   - that the refresh makes an adopted-mode peer safe from eviction. At the host's socket
 *     ceiling SOME session is still closed; what is pinned here is only that the peer this
 *     link is actively pushing to stops being the one the victim search picks first.
 *   - that the stack probe prevents an overflow. It samples a mark that is already a
 *     historical minimum; it names a cause, and the check below measures exactly that
 *     naming (one report, then silence) and nothing more.
 *   - anything about the real `uxTaskGetStackHighWaterMark`. A host thread's unused stack
 *     carries no scannable fill pattern, so the figure is staged by the fake; what is
 *     measured is the link's use of it.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "fake_httpd.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/** @brief A minimal frame body — the link only has to accept and route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/**
 * @brief Drain the control queue to quiescence, as the httpd task does.
 *
 * The TEST thread is the server task in this suite, so a queued send runs here and the
 * whole suite stays deterministic — the same arrangement the #835 suite uses.
 */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim, which is
 *         both the socket-policy edge and the stack-sample edge). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief One inbound frame on an ALREADY-claimed session — the traffic IDF's own LRU
 *         counter responds to. */
void inbound(int fd) { (void)fake_httpd::instance().deliver_frame(fd, kBody); }

/** @brief Push one frame to @p fd alone, through the directed endpoint `peer_link` hands
 *         out, and let the httpd task drain it. */
void push_to(httpd_ws_link_t& link, int fd) {
    tr::net::transport_t* const to = link.peer_link("fd" + std::to_string(fd));
    if (to == nullptr) {
        check(false, "the directed endpoint resolved");
        return;
    }
    to->send(std::span<const std::byte>(kBody));
    drain();
}

// ---------------------------------------------------------------------------
// 1 — the required stack is a public figure, and it is the one the owning ctor applies.
// ---------------------------------------------------------------------------
void test_required_stack_is_public_and_applied() {
    std::printf("#955 the required httpd stack is readable and applied:\n");
    // The measurement itself (F2b, 2026-07-09: the /unit batch apply overflowed 8 KB and
    // needed ~12 KB). Restated here so a shrink of the constant reddens rather than
    // silently redefining what "required" means.
    check(httpd_ws_link_t::kRequiredHttpdStack >= 12288,
          "kRequiredHttpdStack still carries the measured ~12 KB figure");
    fake_httpd::start_config_slot() = httpd_config_t{};
    auto link = std::make_unique<httpd_ws_link_t>(static_cast<std::uint16_t>(8080), 4, true);
    check(link->ok(), "the port-binding link started");
    const httpd_config_t& cfg = fake_httpd::last_start_config();
    check(cfg.stack_size == httpd_ws_link_t::kRequiredHttpdStack,
          "the owning ctor configures exactly the public figure (one constant, not two)");
    // The other two invariants of the same config, restated so this suite fails if either
    // stops being established where an embedder is being told to establish it by hand.
    check(!cfg.lru_purge_enable, "the owning ctor still disables the LRU purge");
    check(cfg.max_open_sockets > 4, "and still budgets sockets past the peer cap");
    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 2 — a thin httpd stack is NAMED, once.
// ---------------------------------------------------------------------------
void test_thin_stack_is_named_once() {
    std::printf("#955 a thin httpd task stack is named once:\n");
    // The floor the TU derives: the margin kRequiredHttpdStack buys over the size that was
    // measured to overflow (8 KB). Spelled out rather than imported so the comparison's
    // direction is pinned from outside the TU.
    constexpr UBaseType_t kFloor = httpd_ws_link_t::kRequiredHttpdStack - 8192;

    // A healthy task: every claim samples, and none latches.
    fake_stack_high_water_bytes() = 65536;
    fake_stack_high_water_samples() = 0;
    auto healthy = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(620);
    claim(621);
    check(fake_stack_high_water_samples() == 2,
          "a healthy stack is sampled at every claim and never latches");
    healthy.reset();
    fake_httpd::instance().close_all();

    // Exactly at the floor is healthy: the comparison is `free >= floor`, so a task with
    // precisely the margin the measurement bought is not reported.
    fake_stack_high_water_bytes() = kFloor;
    fake_stack_high_water_samples() = 0;
    auto boundary = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(630);
    claim(631);
    check(fake_stack_high_water_samples() == 2, "free == the floor is not thin");
    boundary.reset();
    fake_httpd::instance().close_all();

    // One byte under it IS: the first claim reports, and the link then stops sampling —
    // the answer does not change and the sample is an O(free-stack) scan.
    fake_stack_high_water_bytes() = kFloor - 1;
    fake_stack_high_water_samples() = 0;
    auto thin = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(640);
    check(fake_stack_high_water_samples() == 1, "one byte under the floor is sampled");
    claim(641);
    check(fake_stack_high_water_samples() == 1,
          "and having named it once, the link stops sampling (one report, not one per peer)");
    thin.reset();
    fake_httpd::instance().close_all();
    fake_stack_high_water_bytes() = 65536;  // leave the fake healthy for the rest of the suite
}

// ---------------------------------------------------------------------------
// 3 — a pure subscriber stops being the preferential purge victim (adopted mode).
// ---------------------------------------------------------------------------
void test_push_refreshes_lru_for_a_subscriber() {
    std::printf("#955 a pushed-to subscriber is no longer the LRU victim:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    constexpr int kChatty = 650;      // a peer that keeps SENDING (a polling tab, a REST client)
    constexpr int kSubscriber = 651;  // a peer that subscribed and thereafter only RECEIVES
    claim(kChatty);
    claim(kSubscriber);
    // The chatty peer's inbound frame advances its counter, exactly as httpd_sess_process
    // does on silicon. The subscriber sends nothing after its handshake, so its counter
    // stands still — this is the state the defect describes, and it is reproduced, not
    // assumed.
    inbound(kChatty);
    check(fake_httpd::instance().lowest_lru_fd() == kSubscriber,
          "the receive-only peer is the one httpd_accept_conn would purge first");

    // The link pushes to the subscriber. That is server-initiated traffic, which IDF's LRU
    // counter is structurally blind to — so before #955 this changed nothing at all and the
    // busiest subscriber stayed the victim.
    push_to(*link, kSubscriber);
    check(fake_httpd::instance().lowest_lru_fd() == kChatty,
          "after one delivered push the subscriber is no longer the victim");
    check(fake_httpd::instance().lru_counter(kSubscriber) >
              fake_httpd::instance().lru_counter(kChatty),
          "its counter now leads the peer that only sends");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 4 — owning mode does not pay for it: there, purge is off by construction.
// ---------------------------------------------------------------------------
void test_owning_mode_does_not_refresh() {
    std::printf("#955 the owning link does not refresh what it disabled:\n");
    auto link = std::make_unique<httpd_ws_link_t>(static_cast<std::uint16_t>(8081), 0, true);
    check(link->ok(), "the port-binding link started");
    constexpr int kFd = 660;
    claim(kFd);
    const std::uint64_t before = fake_httpd::instance().lru_counter(kFd);
    push_to(*link, kFd);
    check(fake_httpd::instance().frames_sent() != 0, "the push was delivered");
    check(fake_httpd::instance().lru_counter(kFd) == before,
          "no LRU refresh in owning mode: this link set lru_purge_enable = false itself");
    link.reset();
    fake_httpd::instance().close_all();
}

}  // namespace

int main() {
    std::printf("httpd_ws_link adopted-server host-facts suite (#955):\n");
    test_required_stack_is_public_and_applied();
    test_thin_stack_is_named_once();
    test_push_refreshes_lru_for_a_subscriber();
    test_owning_mode_does_not_refresh();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
