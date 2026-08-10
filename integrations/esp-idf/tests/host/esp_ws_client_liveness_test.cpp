/**
 * @file
 * @brief #957 — the WS *client* link's PEER-DEATH seam: noticing one, and reporting one.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #900/#901 receive-path and #952 teardown suites: the REAL
 * chip translation unit (`integrations/esp-idf/libtracer/esp_ws_client_link.cpp`) is
 * compiled against the host fake of `esp_transport_ws` (fake_esp_transport.hpp). What
 * this suite adds to the fake's model is the ONE thing the other two deliberately leave
 * out — the descriptor number `esp_transport_get_socket` answers, opted into per suite
 * (`fake_ws::set_socket_fd`) and paired with a `--wrap=setsockopt` interposition, so the
 * options a dial applies are a measurement rather than a code reading. No socket is
 * opened and no option reaches the kernel; the fake still serves every byte from its own
 * script (#947).
 *
 * The two halves of #957, on this link:
 *
 *   1. NOTICING. `esp_transport_poll_read` reports "no data this turn" forever for a peer
 *      that vanished without a FIN, so `connected_` stayed true and `ok()` answered true
 *      for a dead peer indefinitely on an idle link. The dial now arms `SO_KEEPALIVE`
 *      plus the idle/interval/count tunables, which is what turns a vanished peer into an
 *      ordinary poll/read error. What is pinned here is that the four options are applied
 *      on the dialed socket, with the enable present — the tunables alone are inert.
 *
 *   2. REPORTING. `transport_t::set_down_notifier` is the one departure seam a
 *      point-to-point link has (`fwd_router_t::add_child` wires it to the eviction of
 *      that child's subscriber edges and label bindings). This link used to fire it from
 *      NOWHERE: peer CLOSE, a read error, a poll error and a failed write were all
 *      silent, so after a peer REBOOT the local node kept producing into a session that
 *      no longer existed. It now fires once per connection that was up.
 *
 * The "once per connection that was up" shape is what the four reporting cases pin
 * together, and each is a COUNT COMPARISON rather than an absolute: a peer CLOSE reports,
 * a second CLOSE after the reconnect reports again (so it is per connection, not
 * once-ever), a destructor reports nothing (a local teardown is not a peer departure),
 * and a dial that never lands reports nothing (there was no connection to bury).
 *
 * What this suite deliberately does NOT claim: that lwIP's probes actually fail a
 * connection to a peer that pulled its power. That is a property of a real stack on real
 * silicon and belongs on the HIL — what is provable here is that the link asks for it.
 */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_esp_transport.hpp"
#include "libtracer_esp/esp_ws_client_link.hpp"

namespace {

using namespace std::chrono_literals;

/** @brief Failed-check counter; main() turns it into the exit status. */
int g_failures = 0;

/** @brief Record one assertion. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Spin until @p pred holds or @p limit elapses. @return whether it held. */
template <typename pred_t>
bool wait_until(pred_t pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/** @brief The descriptor NUMBER the fake hands the link for its dialed connection. It is
 *         not a socket: nothing is opened, and `__wrap_setsockopt` below catches every
 *         option applied to it. Deliberately not a plausible live fd. */
constexpr int kFakeFd = 0x5709;

/** @brief The rx/tx buffer size every case here builds its link with. */
constexpr std::size_t kBufBytes = 64;

/** @brief One recorded `setsockopt` call on @ref kFakeFd. */
struct opt_call_t {
    int level;   /**< @brief Its `level` argument. */
    int optname; /**< @brief Its `optname` argument. */
    int value;   /**< @brief Its first int of option data (0 when shorter). */
};

/** @brief Guards @ref g_opts against the recv thread that fills it. */
std::mutex g_opt_m;
/** @brief Every option the link applied to @ref kFakeFd since @ref reset_opts. */
std::vector<opt_call_t> g_opts;

/** @brief Forget every recorded option (paired with `fake_ws::reset`). */
void reset_opts() {
    const std::lock_guard<std::mutex> lk(g_opt_m);
    g_opts.clear();
}

/** @brief The value the link passed for (@p level, @p optname), or -1 if it never set it.
 *         -1 is unambiguous here: every option this link sets is a positive count, a
 *         positive number of seconds, or the enable 1. */
int opt_value(int level, int optname) {
    const std::lock_guard<std::mutex> lk(g_opt_m);
    for (const opt_call_t& c : g_opts)
        if (c.level == level && c.optname == optname) return c.value;
    return -1;
}

/** @brief Counts the link's departure reports, and reads back as a NUMBER so every
 *         assertion can compare an after against a before. */
class down_counter_t {
   public:
    /** @brief The `transport_t::down_fn_t` body — fired on the link's recv thread. */
    static void fire(void* ctx) {
        static_cast<down_counter_t*>(ctx)->n_.fetch_add(1, std::memory_order_relaxed);
    }

    /** @brief How many departures have been reported so far. */
    [[nodiscard]] int count() const { return n_.load(std::memory_order_relaxed); }

   private:
    std::atomic<int> n_{0};
};

/** @brief A CLOSE frame — what a peer that hangs up politely puts on the wire, and the
 *         one death path a test can script through the fake's frame stream. */
fake_ws::frame_t close_frame() {
    return fake_ws::make_frame(WS_TRANSPORT_OPCODES_CLOSE, /*fin=*/true, /*len=*/2, /*seed=*/0x11);
}

/** @brief Build a link and wait for its first dial. */
std::unique_ptr<tr::net::esp_ws_client_link_t> dialed_link() {
    auto link = std::make_unique<tr::net::esp_ws_client_link_t>(
        "127.0.0.1", 8080, "/ws", /*handshake_headers=*/std::string{}, kBufBytes, kBufBytes, 0);
    check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s), "the link dialed");
    return link;
}

// ---------------------------------------------------------------------------
// 1 — NOTICING: the dial arms keepalive on the socket it just connected.
// ---------------------------------------------------------------------------
void test_dial_arms_keepalive() {
    std::printf("#957 a dial arms keepalive on its socket:\n");
    fake_ws::reset();
    reset_opts();
    fake_ws::set_socket_fd(kFakeFd);
    {
        auto link = dialed_link();
        // The enable is the load-bearing one: without it the three tunables are inert,
        // which is why the link applies them only behind it.
        check(wait_until([] { return opt_value(SOL_SOCKET, SO_KEEPALIVE) == 1; }, 2s),
              "SO_KEEPALIVE was enabled on the dialed socket");
        check(opt_value(IPPROTO_TCP, TCP_KEEPIDLE) > 0, "an idle period was set");
        check(opt_value(IPPROTO_TCP, TCP_KEEPINTVL) > 0, "a probe interval was set");
        check(opt_value(IPPROTO_TCP, TCP_KEEPCNT) > 0, "a probe count was set");
        // The pre-existing per-socket policy is unchanged by #957 — the keepalive group
        // was ADDED to it, not substituted for it.
        check(opt_value(IPPROTO_TCP, TCP_NODELAY) == 1, "and TCP_NODELAY is still applied");
        check(opt_value(SOL_SOCKET, SO_SNDTIMEO) != -1, "and the bounded write is still applied");
    }
}

// ---------------------------------------------------------------------------
// 2 — REPORTING: a peer CLOSE is a departure, and the link re-dials after it.
// ---------------------------------------------------------------------------
void test_peer_close_reports_down() {
    std::printf("#957 a peer CLOSE fires the departure seam:\n");
    fake_ws::reset();
    down_counter_t down;
    {
        auto link = dialed_link();
        link->set_down_notifier(&down_counter_t::fire, &down);
        const int before = down.count();
        const int dials_before = fake_ws::connect_count();
        fake_ws::push_frames({close_frame()});
        check(wait_until([&] { return down.count() > before; }, 5s),
              "the departure was reported (it used to be reported from nowhere)");
        check(wait_until([&] { return fake_ws::connect_count() > dials_before; }, 5s),
              "and the link still re-dialed afterwards");
    }
}

// ---------------------------------------------------------------------------
// 3 — REPORTING: it is per CONNECTION, not once ever.
// ---------------------------------------------------------------------------
void test_second_death_reports_again() {
    std::printf("#957 a second peer CLOSE reports again:\n");
    fake_ws::reset();
    down_counter_t down;
    {
        auto link = dialed_link();
        link->set_down_notifier(&down_counter_t::fire, &down);
        // Sampled BEFORE the first death: the report is made on the way to the re-dial,
        // so a sample taken after it has been observed can already include that re-dial.
        const int dials_before = fake_ws::connect_count();
        fake_ws::push_frames({close_frame()});
        check(wait_until([&] { return down.count() >= 1; }, 5s), "the first departure reported");
        const int after_first = down.count();
        check(wait_until([&] { return fake_ws::connect_count() > dials_before; }, 5s),
              "the link re-dialed");
        fake_ws::push_frames({close_frame()});
        check(wait_until([&] { return down.count() > after_first; }, 5s),
              "the SECOND connection's death reported too");
    }
}

// ---------------------------------------------------------------------------
// 4 — REPORTING: a LOCAL teardown is not a peer departure.
// ---------------------------------------------------------------------------
void test_destructor_reports_nothing() {
    std::printf("#957 a local teardown reports no departure:\n");
    fake_ws::reset();
    down_counter_t down;
    {
        auto link = dialed_link();
        link->set_down_notifier(&down_counter_t::fire, &down);
        check(link->ok(), "the link is up before the teardown");
    }
    // Destroyed while CONNECTED: the destructor clears `connected_`, which is the same
    // state a dead peer leaves behind — the recv loop must tell them apart by `stop_`.
    check(down.count() == 0, "the destructor fired no departure (core's stop_ rule)");
}

// ---------------------------------------------------------------------------
// 5 — REPORTING: a dial that never lands buries nothing.
// ---------------------------------------------------------------------------
void test_failed_dials_report_nothing() {
    std::printf("#957 a dial that never lands reports no departure:\n");
    fake_ws::reset();
    down_counter_t down;
    {
        fake_ws::fail_connects(true);
        auto link = std::make_unique<tr::net::esp_ws_client_link_t>(
            "127.0.0.1", 8080, "/ws", /*handshake_headers=*/std::string{}, kBufBytes, kBufBytes, 0);
        link->set_down_notifier(&down_counter_t::fire, &down);
        check(wait_until([] { return fake_ws::connect_count() >= 2; }, 6s),
              "the link retried its dial at least twice");
        check(!link->ok(), "and never came up");
        check(down.count() == 0, "no departure was reported for a link that was never up");
    }
    fake_ws::fail_connects(false);
}

}  // namespace

/**
 * @brief The link's socket-option seam, interposed (`-Wl,--wrap=setsockopt` on this
 *        target).
 *
 * Only the descriptor NUMBER the fake handed out is this suite's business; anything else
 * in the binary goes to the real syscall, on the same rule the httpd suites' `__wrap_send`
 * follows. Nothing is opened here and nothing reaches the kernel for @ref kFakeFd — the
 * call is recorded and answered 0, which is exactly what a stack that accepts the option
 * does.
 */
extern "C" int __real_setsockopt(int fd, int level, int optname, const void* val, socklen_t len);
extern "C" int __wrap_setsockopt(int fd, int level, int optname, const void* val, socklen_t len) {
    if (fd != kFakeFd) return __real_setsockopt(fd, level, optname, val, len);
    int value = 0;
    if (val != nullptr && len >= static_cast<socklen_t>(sizeof(int)))
        std::memcpy(&value, val, sizeof(int));
    const std::lock_guard<std::mutex> lk(g_opt_m);
    g_opts.push_back(opt_call_t{level, optname, value});
    return 0;
}

int main() {
    std::printf("esp_ws_client_link peer-death suite (#957):\n");
    test_dial_arms_keepalive();
    test_peer_close_reports_down();
    test_second_death_reports_again();
    test_destructor_reports_nothing();
    test_failed_dials_report_nothing();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
