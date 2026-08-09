/**
 * @file
 * @brief #958 — `httpd_ws_link_t::set_admission_cb`'s predicate actually decides the
 *        opening handshake, on the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 / #835 / #954 / #957 suites: the REAL chip translation
 * unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) is compiled against the host
 * fake of `esp_http_server` (fake_httpd.hpp). What this suite adds to those is the
 * HANDSHAKE seam — the fake now runs the registration's `ws_pre_handshake_cb` before it
 * admits a socket into the session table and latches the WS route into it, which is the
 * order `httpd_uri.c` uses. That makes "was the predicate consulted, and did its answer
 * decide whether a session exists" a measurement rather than a code reading.
 *
 * Why the seam had to move at all: `esp_http_server` answers the WebSocket handshake
 * itself and returns from `httpd_uri()` BEFORE calling `uri->handler`, so a predicate
 * placed in the URI handler is never handed the opening GET. The peer is claimed on its
 * first data frame instead, and a predicate that only runs there has already lost — the
 * upgrade is done. `ws_pre_handshake_cb` is the one call that gets the parsed GET.
 *
 * The four things pinned here:
 *   1. the predicate is consulted at the handshake, with the opening GET (method GET, the
 *      peer's socket) and the ctx registered alongside it;
 *   2. a refusal means NO session, NO latched route, and no peer — a later frame on that
 *      descriptor finds nothing to dispatch through;
 *   3. an admitted peer is unaffected: it claims its slot on its first frame and appears
 *      in `enumerate_peers` exactly as before;
 *   4. BOTH registration sites carry the thunk — the own-server ctor and the
 *      adopted-server ctor — because a fix on one of them is a hole on the other.
 *
 * What this suite deliberately does NOT claim: anything about HEADER inspection. A real
 * predicate reads `httpd_req_get_hdr_value_str` (a cookie, a bearer token) and the fake
 * models no header store, so what is provable here is WHERE the predicate runs and what
 * its verdict does — not that a particular credential check works. That belongs on
 * silicon. Nor does it claim anything about TLS, origins, or any other admission policy:
 * the policy is the host's, and this link's whole contract is that it asks.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fake_httpd.hpp"
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

/** @brief A minimal frame body — the link only has to accept it and claim the peer. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief One recorded call of the admission predicate. */
struct call_t {
    void* ctx;  /**< @brief The opaque pointer it was handed. */
    int fd;     /**< @brief The socket the handshake is for. */
    int method; /**< @brief `httpd_req_t::method` — GET on the opening request. */
};

/** @brief Every predicate call since @ref reset_hook. */
std::vector<call_t> g_calls;
/** @brief What the predicate answers. */
bool g_verdict = true;

/** @brief Forget every recorded call and set the verdict for the next ones. */
void reset_hook(bool verdict) {
    g_calls.clear();
    g_verdict = verdict;
}

/** @brief The predicate under test: record the call, answer @ref g_verdict. */
bool recording_hook(void* ctx, httpd_req_t* req) {
    g_calls.push_back(call_t{ctx, httpd_req_to_sockfd(req), req->method});
    return g_verdict;
}

/** @brief How many peers the link currently exposes through the bus facet. */
std::size_t peer_count(const httpd_ws_link_t& link) {
    std::size_t n = 0;
    link.enumerate_peers([&n](std::string_view) { ++n; });
    return n;
}

/** @brief The predicate decides the handshake, and its refusal leaves nothing behind. */
void test_refusal_stops_the_upgrade() {
    std::printf("#958 a refused peer never becomes a peer:\n");
    int ctx_object = 0;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    link->set_admission_cb(&recording_hook, &ctx_object);
    reset_hook(false);
    constexpr int kFd = 510;

    const bool opened = fake_httpd::instance().open_session(kFd);
    check(g_calls.size() == 1, "the predicate was consulted exactly once, at the handshake");
    if (g_calls.size() == 1) {
        check(g_calls[0].method == HTTP_GET, "and it was handed the opening GET");
        check(g_calls[0].fd == kFd, "for the peer's own socket");
        check(g_calls[0].ctx == &ctx_object, "with the ctx registered alongside it");
    }
    check(!opened, "the handshake was abandoned — no session was admitted");
    check(!fake_httpd::instance().has_session(kFd),
          "and the descriptor has no session table entry");
    // The teeth. Before the thunk the predicate was installed where the opening GET never
    // arrives, so the upgrade completed and the peer was claimed on its first frame; a
    // refusal changed nothing. Now there is no route to dispatch that frame through.
    check(fake_httpd::instance().deliver_frame(kFd, kBody) == ESP_ERR_NOT_FOUND,
          "a frame on that descriptor finds no latched route");
    check(peer_count(*link) == 0, "and the refused peer is in no peer set");
}

/** @brief The default and the admitting verdict both leave the peer path intact. */
void test_admission_still_works() {
    std::printf("#958 an admitted peer is unaffected:\n");
    int ctx_object = 0;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    link->set_admission_cb(&recording_hook, &ctx_object);
    reset_hook(true);
    constexpr int kFd = 520;

    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    check(g_calls.size() == 1, "the predicate was consulted once");
    check(fake_httpd::instance().deliver_frame(kFd, kBody) == ESP_OK, "its first frame dispatched");
    check(peer_count(*link) == 1, "and it claimed a peer slot");
    check(g_calls.size() == 1, "the data frame did NOT re-consult the predicate");
    fake_httpd::instance().close_session(kFd);
}

/** @brief No predicate registered admits every peer — the historical open graph. */
void test_no_hook_admits() {
    std::printf("#958 an unset predicate admits every peer:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    reset_hook(false);  // the verdict is irrelevant: nothing is installed
    constexpr int kFd = 530;
    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    check(g_calls.empty(), "with no predicate consulted");
    check(fake_httpd::instance().deliver_frame(kFd, kBody) == ESP_OK, "and the peer was claimed");
    check(peer_count(*link) == 1, "so an unconfigured link still serves every peer");
    fake_httpd::instance().close_session(kFd);
}

/** @brief The OWN-SERVER ctor registers the same thunk — a fix on one site only is a hole
 *         on the other. */
void test_owning_ctor_gates_too() {
    std::printf("#958 the own-server constructor gates its handshake too:\n");
    int ctx_object = 0;
    auto link = std::make_unique<httpd_ws_link_t>(static_cast<std::uint16_t>(8090), 0, true);
    check(link->ok(), "the port-binding link started its server and registered its URI");
    link->set_admission_cb(&recording_hook, &ctx_object);
    reset_hook(false);
    constexpr int kFd = 540;
    check(!fake_httpd::instance().open_session(kFd), "the handshake was abandoned");
    check(g_calls.size() == 1, "the predicate was consulted at that handshake");
    check(peer_count(*link) == 0, "and no peer was created");
}

}  // namespace

int main() {
    std::printf("httpd_ws_link admission suite (#958):\n");
    test_refusal_stops_the_upgrade();
    test_admission_still_works();
    test_no_hook_admits();
    test_owning_ctor_gates_too();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
