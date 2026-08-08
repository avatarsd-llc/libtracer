/**
 * @file
 * @brief #957 — the httpd WS link NOTICES a peer that vanishes without a FIN, on the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 / #835 / #954 suites: the REAL chip translation unit
 * (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) is compiled against the host fake
 * of `esp_http_server` (fake_httpd.hpp). What this suite adds to those is the SOCKET
 * OPTION seam — `setsockopt` is interposed (`-Wl,--wrap=setsockopt`) for the descriptors
 * the fake hands out, so what a session's admission applies to its socket is a
 * measurement rather than a code reading.
 *
 * The defect: enumerating the ways an inbound session can end gives peer CLOSE/FIN, a
 * handler returning `ESP_FAIL`, the TX failure streak, the short-write condemn, and
 * `httpd_stop`. There is no timer among them, and both constructors set
 * `lru_purge_enable = false` on purpose (the admission contract: never evict a LIVE peer
 * to make room for a new one), which removes the one reclaim `esp_http_server` has for an
 * idle socket. The TX streak cannot substitute — `note_tx_result` is fed only from
 * `tx_work`, so a session the link never sends to accrues no evidence at all. So a peer
 * that disappeared without a FIN held its slot AND one unit of `max_peers` for the life
 * of the process. Keepalive probes are what turn that into an ordinary socket error, on
 * which httpd closes the session and the existing free_ctx → reclaim_slot →
 * notify_peer_down path runs.
 *
 * What is pinned here: admission applies the enable AND the three tunables to the peer's
 * own socket, and does not disturb the per-socket policy that was already there (#835's
 * bounded write, the Nagle disable). The enable is checked separately from the tunables
 * because the tunables are inert without it — which is why the link applies them only
 * behind it.
 *
 * What this suite deliberately does NOT claim: that lwIP's probes actually fail a
 * connection to a peer that pulled its power, or how long that takes. That is a real
 * stack on real silicon and belongs on the HIL; what is provable here is that the link
 * asks for it rather than trusting the host server's `keep_alive_enable`.
 */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
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

/** @brief One recorded `setsockopt` call on a fake-owned descriptor. */
struct opt_call_t {
    int fd;      /**< @brief The descriptor it was applied to. */
    int level;   /**< @brief Its `level` argument. */
    int optname; /**< @brief Its `optname` argument. */
    int value;   /**< @brief Its first int of option data (0 when shorter). */
};

/** @brief Guards @ref g_opts. */
std::mutex g_opt_m;
/** @brief Every option applied to a fake-owned descriptor since @ref reset_opts. */
std::vector<opt_call_t> g_opts;

/** @brief Forget every recorded option. */
void reset_opts() {
    const std::lock_guard<std::mutex> lk(g_opt_m);
    g_opts.clear();
}

/** @brief The value applied for (@p fd, @p level, @p optname), or -1 if never set. -1 is
 *         unambiguous here: every option this link sets is a positive count, a positive
 *         number of seconds, or the enable 1. */
int opt_value(int fd, int level, int optname) {
    const std::lock_guard<std::mutex> lk(g_opt_m);
    for (const opt_call_t& c : g_opts)
        if (c.fd == fd && c.level == level && c.optname == optname) return c.value;
    return -1;
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim, which is
 *         the admission edge this fake drives). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief Admission arms keepalive on the peer's own socket. */
void test_admission_arms_keepalive() {
    std::printf("#957 admission arms keepalive on the peer's socket:\n");
    reset_opts();
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    constexpr int kFd = 410;
    check(opt_value(kFd, SOL_SOCKET, SO_KEEPALIVE) == -1,
          "nothing was applied to the socket before it was admitted");
    claim(kFd);
    check(opt_value(kFd, SOL_SOCKET, SO_KEEPALIVE) == 1,
          "SO_KEEPALIVE was enabled at admission (nothing else times an idle session out)");
    check(opt_value(kFd, IPPROTO_TCP, TCP_KEEPIDLE) > 0, "an idle period was set");
    check(opt_value(kFd, IPPROTO_TCP, TCP_KEEPINTVL) > 0, "a probe interval was set");
    check(opt_value(kFd, IPPROTO_TCP, TCP_KEEPCNT) > 0, "a probe count was set");
    // The pre-existing per-socket policy is unchanged by #957 — the keepalive group was
    // ADDED to it, not substituted for it.
    check(opt_value(kFd, IPPROTO_TCP, TCP_NODELAY) == 1, "and TCP_NODELAY is still applied");
    check(opt_value(kFd, SOL_SOCKET, SO_SNDTIMEO) != -1,
          "and the #835 send bound is still applied");
}

/** @brief Each admitted peer gets its own — the policy is per socket, not per link. */
void test_every_admitted_peer_gets_it() {
    std::printf("#957 a second peer gets its own keepalive:\n");
    reset_opts();
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    claim(420);
    claim(421);
    check(opt_value(420, SOL_SOCKET, SO_KEEPALIVE) == 1, "peer 420 is covered");
    check(opt_value(421, SOL_SOCKET, SO_KEEPALIVE) == 1, "peer 421 is covered");
    check(opt_value(421, IPPROTO_TCP, TCP_KEEPCNT) > 0, "with its own probe count");
}

}  // namespace

/**
 * @brief The link's socket-option seam, interposed (`-Wl,--wrap=setsockopt` on this
 *        target).
 *
 * Only descriptors the fake handed out are this suite's business; anything else in the
 * binary goes to the real syscall, on the same rule `__wrap_send` follows. The fake's
 * descriptors are plain numbers, so the real `setsockopt` would only ever return EBADF on
 * them — recording the call and answering 0 is both what a stack that accepts the option
 * does and the only way to see WHICH options were asked for.
 */
extern "C" int __real_setsockopt(int fd, int level, int optname, const void* val, socklen_t len);
extern "C" int __wrap_setsockopt(int fd, int level, int optname, const void* val, socklen_t len) {
    if (!fake_httpd::instance().owns_socket(fd))
        return __real_setsockopt(fd, level, optname, val, len);
    int value = 0;
    if (val != nullptr && len >= static_cast<socklen_t>(sizeof(int)))
        std::memcpy(&value, val, sizeof(int));
    const std::lock_guard<std::mutex> lk(g_opt_m);
    g_opts.push_back(opt_call_t{fd, level, optname, value});
    return 0;
}

int main() {
    std::printf("httpd_ws_link keepalive suite (#957):\n");
    test_admission_arms_keepalive();
    test_every_admitted_peer_gets_it();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
