/**
 * @file
 * @brief Transport seam-conformance suite.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The `transport_t` contract — send/receive
 * delivery, byte-identity, scatter-gather gather-send, bidirectional delivery,
 * multi-frame delivery, and the ADR-0042 owning-view seam — was re-proved bespoke
 * in each per-transport *_test.cpp. This drives ONE parameterized contract body
 * over every point-to-point adapter (in-process loopback + the real localhost
 * udp / tcp / ws sockets), so the seam is a single test surface: a new transport
 * proves the contract by adding a pair adapter, and loopback (previously reached
 * only indirectly) and ws gain first-class seam coverage.
 *
 * Transport-SPECIFIC behavior (udp pool-exhaustion backpressure, zero-copy store,
 * ws fragment reassembly, the CAN bus facet) stays in the per-transport tests;
 * this file proves only what every point-to-point transport_t must do uniformly.
 * Built under TSan (recv thread + receiver handoff) and ASan+UBSan.
 */

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/loopback.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::transport_t;

using tr::testing::check;

using bytes_t = std::vector<std::byte>;

bytes_t frame_of(std::initializer_list<std::uint8_t> bytes) {
    bytes_t v;
    for (std::uint8_t b : bytes) v.push_back(std::byte{b});
    return v;
}

/**
 * @brief A thread-safe sink that records each delivered frame's bytes and lets a test wait for an
 *        expected count (delivery lands on the transport's recv thread).
 */
struct collector_t {
    std::mutex m;
    std::condition_variable cv;
    std::vector<bytes_t> frames;

    void on_frame(std::span<const std::byte> f) {
        {
            const std::lock_guard lock(m);
            frames.emplace_back(f.begin(), f.end());
        }
        cv.notify_all();
    }
    [[nodiscard]] bool wait_for(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
    [[nodiscard]] std::size_t count() {
        const std::lock_guard lock(m);
        return frames.size();
    }
    [[nodiscard]] bytes_t at(std::size_t i) {
        const std::lock_guard lock(m);
        return frames.at(i);
    }
    void clear() {
        const std::lock_guard lock(m);
        frames.clear();
    }
};

/**
 * @brief The ADR-0042 owning-view seam's capture state (a rope receiver holds an OWNING view past
 *        the callback).
 *
 * Like @ref collector_t it must outlive the pair whose
 * recv thread delivers into it.
 */
struct owning_capture_t {
    std::mutex m;
    std::condition_variable cv;
    bool got = false;
    tr::view::view_t view;

    void on_rope(tr::view::rope_t f) {
        if (f.link_count() != 1) return;  // this contract sends single-frame ropes
        const std::lock_guard lock(m);
        view = f.links()[0];  // hold the owning view past the callback
        got = true;
        cv.notify_all();
    }
    [[nodiscard]] bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return got; });
    }
};

// ---- Pair adapters: each sets up a connected a()->b() point-to-point link -----
// `a()` is the sender endpoint, `b()` the receiver; each adapter exposes the
// concrete-type ok() (transport_t has no ok() — it is a per-transport method) and
// tears the link down (joining recv threads) before the collector it delivered to.

struct loopback_pair {
    tr::net::loopback_channel_t ch;
    ~loopback_pair() { ch.shutdown(); }  // join recv threads before receivers die
    [[nodiscard]] bool ok() const { return true; }
    [[nodiscard]] transport_t& a() { return ch.a(); }
    [[nodiscard]] transport_t& b() { return ch.b(); }
    static constexpr const char* name = "loopback";
};

/**
 * @brief The `udp` contract pair — both endpoints bind an EPHEMERAL port (#1362).
 *
 * No port number is reserved for tests: the fixed 47xxx literals these used to name sit
 * inside the kernel's default `ip_local_port_range` (32768-60999), so an ordinary client
 * socket can already own one and `bind(2)` then fails EADDRINUSE. Port 0 removes the
 * contention rather than narrowing the window — the OS grants a free port and `local_port()`
 * reports it. `b_` therefore binds FIRST, in learn-peer mode (empty host), and `a_` aims at
 * the port `b_` actually got.
 *
 * Ordering dependency worth knowing: `b_` has no peer until an inbound datagram teaches it
 * one, so the contract's `b()`→`a()` reverse leg only works because barriers (1) and (2)
 * have already confirmed a→b traffic by the time it runs.
 */
struct udp_pair {
    tr::net::udp_transport_t b_{0, "", 0};
    tr::net::udp_transport_t a_{0, "127.0.0.1", b_.local_port()};
    [[nodiscard]] bool ok() const { return a_.ok() && b_.ok(); }
    [[nodiscard]] transport_t& a() { return a_; }
    [[nodiscard]] transport_t& b() { return b_; }
    static constexpr const char* name = "udp";
};

struct tcp_pair {
    /**
     * @brief Declaration order = construction order: the listener binds first (port 0, see
     *        @ref udp_pair for why nothing here uses a fixed port), then the dialer's synchronous
     *        connect to the granted `local_port()` completes the link.
     */
    tr::net::tcp_transport_t listener_{0};
    tr::net::tcp_transport_t dialer_{"127.0.0.1", listener_.local_port()};
    [[nodiscard]] bool ok() const { return listener_.ok() && dialer_.ok(); }
    [[nodiscard]] transport_t& a() { return dialer_; }
    [[nodiscard]] transport_t& b() { return listener_; }
    static constexpr const char* name = "tcp";
};

struct ws_pair {
    tr::net::ws_server_transport_t server_{0}; /**< binds an ephemeral port (see @ref udp_pair) */
    tr::net::ws_client_transport_t client_{"127.0.0.1",
                                           server_.local_port()}; /**< handshake in the ctor */
    [[nodiscard]] bool ok() const { return server_.ok() && client_.ok(); }
    [[nodiscard]] transport_t& a() { return client_; }
    [[nodiscard]] transport_t& b() { return server_; }
    static constexpr const char* name = "ws";
};

constexpr auto kWait = 3s;

// ---- The one contract body, run over each adapter --------------------------

template <class Pair>
void run_contract() {
    std::printf("  transport_t contract — %s:\n", Pair::name);

    // Every receiver delivers into `coll` / `own`; the pair's dtor joins the recv
    // threads, so this shared state is declared FIRST and destroyed LAST — a recv
    // thread can never touch a dead collector. Each check is a wait_for barrier, so
    // one check's frames are fully drained before the next clears and reuses `coll`.
    collector_t coll;
    owning_capture_t own;
    // The named receiver lambdas live with their collectors, BEFORE the pair:
    // the slot binds each callable by address, so it too must outlive delivery.
    auto to_coll = [&coll](std::span<const std::byte> f) { coll.on_frame(f); };
    auto to_own = [&own](tr::view::rope_t f) { own.on_rope(std::move(f)); };
    Pair pair;
    if (!pair.ok()) {
        check(false, "endpoints did not come up (ok())");
        return;
    }
    check(true, "both endpoints report ok()");
    // A dialling link spawns its receive thread only when armed (#2247); a no-op elsewhere.
    pair.a().start_receiving();
    pair.b().start_receiving();

    // 1. A single frame sent a()->b() arrives byte-identical.
    pair.b().set_receiver(to_coll);
    const bytes_t one = frame_of({0x09, 0xAB, 0xCD, 0xEF, 0x42});
    pair.a().send(one);
    check(coll.wait_for(1, kWait), "single frame delivered a->b");
    check(coll.count() >= 1 && coll.at(0) == one, "delivered bytes are byte-identical");

    // 2. Scatter-gather send(iov) arrives as one concatenated frame (native
    //    sendmsg/writev where supported; the base gather-then-send otherwise).
    coll.clear();
    {
        const bytes_t s0 = frame_of({0x01, 0x02});
        const bytes_t s1 = frame_of({0x03, 0x04, 0x05});
        const bytes_t s2 = frame_of({0x06});
        const std::array<std::span<const std::byte>, 3> iov{std::span<const std::byte>(s0),
                                                            std::span<const std::byte>(s1),
                                                            std::span<const std::byte>(s2)};
        pair.a().send(std::span<const std::span<const std::byte>>(iov));
    }
    check(coll.wait_for(1, kWait), "scatter-gather frame delivered");
    check(coll.count() >= 1 && coll.at(0) == frame_of({0x01, 0x02, 0x03, 0x04, 0x05, 0x06}),
          "gathered segments arrive concatenated as one frame");

    // 3. Bidirectional: a frame b()->a() arrives too (the FWD reply plane relies on
    //    the return leg of the same link). Safe after (1): b has accepted/served.
    pair.a().set_receiver(to_coll);
    coll.clear();
    const bytes_t rev = frame_of({0x55, 0x66, 0x77});
    pair.b().send(rev);
    check(coll.wait_for(1, kWait), "single frame delivered b->a (bidirectional)");
    check(coll.count() >= 1 && coll.at(0) == rev, "reverse-direction bytes are byte-identical");

    // 4. Multiple distinct frames a()->b() all arrive (datagram order is not
    //    protocol-guaranteed, so assert the delivered SET, not the sequence).
    coll.clear();
    {
        const std::array<bytes_t, 3> sent{frame_of({0xA0}), frame_of({0xB1, 0xB2}),
                                          frame_of({0xC3, 0xC4, 0xC5})};
        for (const bytes_t& f : sent) pair.a().send(f);
        check(coll.wait_for(3, kWait), "all three frames delivered");
        bool all_present = coll.count() == 3;
        for (const bytes_t& s : sent) {
            bool found = false;
            for (std::size_t i = 0; i < coll.count(); ++i)
                if (coll.at(i) == s) found = true;
            all_present = all_present && found;
        }
        check(all_present, "every sent frame arrived exactly once (byte-identical)");
    }

    // 5. ADR-0042 owning-view seam — only for transports that advertise it. A frame
    //    arrives as a rope whose single link OWNS a refcounted segment (the receiver
    //    may keep it beyond the callback), byte-identical to what was sent.
    if (pair.b().delivers_ropes()) {
        pair.b().set_rope_receiver(to_own);
        const bytes_t frame = frame_of({0x11, 0x22, 0x33, 0x44});
        pair.a().send(frame);
        const bool arrived = own.wait(kWait);
        check(arrived, "owning frame delivered to the rope receiver");
        check(arrived && static_cast<bool>(own.view.owner),
              "the single link OWNS a refcounted segment");
        if (arrived) {
            const auto b = own.view.bytes();
            check(b.size() == frame.size() && bytes_t(b.begin(), b.end()) == frame,
                  "owning frame bytes are byte-identical (narrowed to the frame length)");
        }
    } else {
        check(true, "span-only transport (no owning-view seam) — contract N/A");
    }
}

}  // namespace

/** @brief A source that refuses every draw. */
/**
 * @brief A source that serves its first @p budget draws from the default root and refuses
 *        every later one, counting both (#2288). A budget of 0 refuses everything.
 */
class budget_source_t final : public tr::mem::block_source_t {
   public:
    explicit budget_source_t(std::size_t budget) noexcept
        : block_source_t("test_budget_src"), left_(budget) {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        const std::lock_guard lock(m_);
        if (left_ == 0) {
            ++refused_;
            return nullptr;
        }
        void* const p = tr::mem::default_root().try_alloc(bytes, align);
        if (p == nullptr) return nullptr;
        --left_;
        ++served_;
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::default_root().release(p, bytes, align);
    }
    [[nodiscard]] std::size_t served() const {
        const std::lock_guard lock(m_);
        return served_;
    }
    [[nodiscard]] std::size_t refused() const {
        const std::lock_guard lock(m_);
        return refused_;
    }

   private:
    mutable std::mutex m_;
    std::size_t left_;
    std::size_t served_ = 0;
    std::size_t refused_ = 0;
};

/**
 * @brief #2288 — a loopback frame whose block the source refuses is a COUNTED drop.
 *
 * The source serves the two endpoints' thread records and refuses from then on, so both
 * endpoints are running and the drops isolate the refused-frame path, not "peer not running".
 */
void test_loopback_refused_frame_is_counted() {
    std::printf("loopback: a refused frame block is counted in dropped_tx:\n");
    budget_source_t src(2);
    tr::net::loopback_channel_t ch(src);
    check(src.served() == 2 && src.refused() == 0, "both receive threads were spawned");
    const auto frame = frame_of({0x01, 0x02, 0x03});
    ch.a().send(frame);
    ch.a().send(frame);
    check(src.refused() == 2, "each frame's block was asked for and refused");
    check(ch.a().drop_stats().dropped_tx == 2, "both refused frames were counted");
    check(ch.b().drop_stats().dropped_tx == 0, "the receiving end counted nothing");
}

/**
 * @brief #2288 — every socket link draws its receive thread from `memory.stack`, and a
 *        refusal there is reported: a listener or a udp link is inert (`ok()` false), a dial
 *        link's `start_receiving()` answers false and the link is closed, never up and deaf.
 */
void test_refused_receive_spawn_is_reported() {
    std::printf("socket links: a refused receive-thread spawn is reported:\n");
    budget_source_t none(0);
    const tr::net::link_memory_t refused{.stack = &none};

    const tr::net::udp_transport_t udp(0, "", 0, tr::net::udp_config_t{.memory = refused});
    check(!udp.ok(), "udp: ok() is false");
    const tr::net::tcp_transport_t tcp_listen(0, tr::net::tcp_config_t{.memory = refused});
    check(!tcp_listen.ok(), "tcp listener: ok() is false");
    const tr::net::tcp_server_transport_t tcp_server(
        0, tr::net::tcp_server_config_t{.memory = refused});
    check(!tcp_server.ok(), "tcp server: ok() is false");
    const tr::net::ws_server_transport_t ws_server(0,
                                                   tr::net::ws_server_config_t{.memory = refused});
    check(!ws_server.ok(), "ws server: ok() is false");

    // The dials need a live peer to come up at all; their peers draw from the default.
    tr::net::tcp_transport_t tcp_peer(0);
    tr::net::tcp_transport_t tcp_dial("127.0.0.1", tcp_peer.local_port(),
                                      tr::net::tcp_config_t{.memory = refused});
    check(tcp_dial.ok(), "tcp dial: came up before its receive was asked for");
    check(!tcp_dial.start_receiving(), "tcp dial: start_receiving() answers false");
    check(!tcp_dial.ok() && !tcp_dial.link_up(), "tcp dial: closed, ok() and link_up() false");
    tr::net::ws_server_transport_t ws_peer(0);
    tr::net::ws_client_transport_t ws_dial("127.0.0.1", ws_peer.local_port(),
                                           tr::net::ws_client_config_t{.memory = refused});
    check(ws_dial.ok(), "ws dial: came up before its receive was asked for");
    check(!ws_dial.start_receiving(), "ws dial: start_receiving() answers false");
    check(!ws_dial.ok() && !ws_dial.link_up(), "ws dial: closed, ok() and link_up() false");
    check(none.refused() == 6, "each of the six spawns asked the stack source and was refused");
}

/**
 * @brief #2288 — the socket set a full node brings up (udp listener and dial, tcp listener
 *        and dial, ws server and dial) spawns every receive thread from `memory.stack`, and
 *        none draws from the link's frame or state planes for it.
 */
void test_full_socket_set_spawns_every_receive() {
    std::printf("socket links: the full set spawns every receive thread from memory.stack:\n");
    budget_source_t stacks(64);
    const tr::net::link_memory_t mem{.stack = &stacks};
    tr::net::udp_transport_t udp_listen(0, "", 0, tr::net::udp_config_t{.memory = mem});
    tr::net::udp_transport_t udp_dial(0, "127.0.0.1", udp_listen.local_port(),
                                      tr::net::udp_config_t{.memory = mem});
    tr::net::tcp_transport_t tcp_listen(0, tr::net::tcp_config_t{.memory = mem});
    tr::net::tcp_transport_t tcp_dial("127.0.0.1", tcp_listen.local_port(),
                                      tr::net::tcp_config_t{.memory = mem});
    tr::net::ws_server_transport_t ws_server(0, tr::net::ws_server_config_t{.memory = mem});
    tr::net::ws_client_transport_t ws_dial("127.0.0.1", ws_server.local_port(),
                                           tr::net::ws_client_config_t{.memory = mem});
    check(tcp_dial.start_receiving() && ws_dial.start_receiving(), "both dials started");
    check(udp_listen.ok() && udp_dial.ok() && tcp_listen.ok() && tcp_dial.ok() && ws_server.ok() &&
              ws_dial.ok(),
          "every link is up");
    check(stacks.served() == 6 && stacks.refused() == 0,
          "six receive threads, each drawn from memory.stack, none refused");
    check(tcp_dial.start_receiving() && ws_dial.start_receiving(),
          "a repeat start is a no-op that answers true");
    check(stacks.served() == 6, "and spawns nothing more");
}

int main() {
    std::printf("transport_t seam conformance — one contract over every adapter:\n");
    test_loopback_refused_frame_is_counted();
    test_refused_receive_spawn_is_reported();
    test_full_socket_set_spawns_every_receive();
    run_contract<loopback_pair>();
    run_contract<udp_pair>();
    run_contract<tcp_pair>();
    run_contract<ws_pair>();
    return tr::testing::summary("transport_conformance");
}
