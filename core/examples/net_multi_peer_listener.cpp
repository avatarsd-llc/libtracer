/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a LISTEN link is not one link per peer: `transport_tcp_server` holds
 *        many peers in recycled SLOTS behind one `transport_t`, names them POSITIONALLY as
 *        `p<slot>`, and that positional naming is precisely why a resolved peer endpoint
 *        must be re-resolved per use instead of cached.
 *
 * `tcp_transport_t`'s LISTEN constructor accepts one peer at a time — the board↔board
 * shape. A node that fans out to browser tabs or to a fleet needs the other one:
 * `transport_tcp_server` (and its RFC 6455 sibling `transport_ws_server`) share one
 * poll thread over a slot table, so steady-state memory is bounded by the concurrent-peer
 * high-water mark or by `max_peers`, whichever is smaller — an injected bound (RFC-0006),
 * never a synthetic backlog. A connection past the cap is accepted and immediately closed,
 * which is a clean refusal rather than a hung SYN.
 *
 * With `peer_named`, that slot table is exposed through the same @ref tr::net::bus_link_t
 * facet CAN uses, and this is where the two naming regimes part company:
 *
 *  - CAN names a peer `n<node-id>` — an IDENTITY. The name, the table key and the endpoint
 *    are one thing that no other peer can inherit.
 *  - A slot server names a peer `p<slot>` — a POSITION. The endpoint is scoped to the SLOT,
 *    so after that peer departs a pointer resolved for it addresses whatever session
 *    inherits the slot, and the endpoint's own liveness check is satisfied by that stranger.
 *    The pointer never dangles; it silently changes who it means (#1153). Resolve, send,
 *    discard — `child_registry_t` does exactly that in one expression, which is why no
 *    shipping caller is exposed.
 *
 * The subject of this example is the ADR-0044 peer-named tier, so a target that closed the
 * bus module out (`kBusLinks = false`) does not contain it. That is stated and SKIPPED with
 * exit code 77 — which ctest reads as `Skipped`, not as a pass — rather than returning 0
 * from a `main` that demonstrated nothing.
 *
 * Needs the TCP transport (`LIBTRACER_TRANSPORT_TCP`, on by default). Runs under ctest as
 * `example_net_multi_peer_listener`; returns non-zero on any failed check.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/config.hpp"
#include "libtracer/transport_tcp.hpp"

namespace {

using namespace std::chrono_literals;

/**
 * @brief The exit code ctest reads as SKIPPED, via the target's `SKIP_RETURN_CODE` property.
 *
 * 77 is the autotools convention ctest documents. The same value `tr::testing::kSkipExitCode`
 * spells on the test side; the `add_test` site in `core/examples/CMakeLists.txt` and this
 * constant have to agree, so changing one means changing both.
 */
constexpr int kSkipExitCode = 77;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A raw POSIX TCP client — one peer of the listener, with its own eye on the wire. */
class raw_peer_t {
   public:
    /** @brief Connect to `127.0.0.1:@p port`; @ref ok reports whether it succeeded. */
    explicit raw_peer_t(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~raw_peer_t() {
        if (fd_ >= 0) ::close(fd_);
    }

    raw_peer_t(const raw_peer_t&) = delete;
    raw_peer_t& operator=(const raw_peer_t&) = delete;

    /** @brief True iff the connect succeeded. */
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    /** @brief Read up to @p want bytes within @p budget, answering whatever arrived. */
    [[nodiscard]] std::vector<std::byte> read_within(std::size_t want,
                                                     std::chrono::milliseconds budget) {
        std::vector<std::byte> got;
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (got.size() < want) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
            std::byte buf[256];
            const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.insert(got.end(), buf, buf + n);
        }
        return got;
    }

   private:
    int fd_ = -1;
};

/** @brief @p n bytes counting up from @p seed — a stand-in for an encoded frame. */
std::vector<std::byte> frame_of(std::size_t n, unsigned seed) {
    std::vector<std::byte> f(n);
    for (std::size_t i = 0; i < n; ++i) f[i] = static_cast<std::byte>(seed + i);
    return f;
}

/** @brief `u32-LE length ++ @p payload` — what one record looks like on this kind's wire. */
std::vector<std::byte> record(std::span<const std::byte> payload) {
    const auto len = static_cast<std::uint32_t>(payload.size());
    std::vector<std::byte> out;
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((len >> shift) & 0xFFu));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/** @brief The peer names @p facet currently lists, sorted so the order is this example's. */
std::vector<std::string> peers_of(tr::net::bus_link_t& facet) {
    std::vector<std::string> names;
    facet.enumerate_peers([&](std::string_view n) { names.emplace_back(n); });
    std::sort(names.begin(), names.end());
    return names;
}

/**
 * @brief Poll until @p facet lists @p n peers, or @p budget expires.
 *
 * A bounded poll, not a wait: acceptance happens on the server's own poll thread and the
 * seam publishes no completion edge to wait on. The loop is the honest shape for observing
 * a state that becomes true asynchronously with no notification.
 */
bool wait_for_peers(tr::net::bus_link_t& facet, std::size_t n, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (peers_of(facet).size() >= n) return true;
        std::this_thread::sleep_for(2ms);
    }
    return peers_of(facet).size() >= n;
}

}  // namespace

int main() {
    // The subject is the peer-named tier itself. On a target that compiled it out there is
    // nothing here to demonstrate, and a `return 0` would make ctest record a pass for an
    // example that ran nothing — the failure mode this whole batch exists to avoid.
    if constexpr (!tr::net::kBusLinks) {
        std::printf(
            "net_multi_peer_listener: SKIPPED — this build closed the ADR-0044 bus module "
            "out (kBusLinks = false), so a listener has no peer-named tier to show.\n");
        return kSkipExitCode;
    }

    bool ok = true;
    std::printf("kBusLinks = true — the peer-named tier is present in this build\n");

    // max_peers is the injected admission bound. It is resolved once at construction (a
    // request of 0 takes the liveness window's own ceiling), so the value the server ENFORCES
    // is read back rather than assumed to be what was asked for.
    constexpr std::size_t kRequestedPeers = 4;
    tr::net::transport_tcp_server server(std::uint16_t{0}, &tr::mem::heap_backend(),
                                         /*max_frame=*/0, kRequestedPeers, /*peer_named=*/true);
    check(ok, server.ok(), "the multi-peer listener bound an ephemeral port");
    check(ok, server.max_peers() == kRequestedPeers, "the admission cap is the one requested");
    check(ok, server.bus() != nullptr, "peer_named=true exposes the bus facet");

    tr::net::bus_link_t& facet = *server.bus();
    check(ok, peers_of(facet).empty(), "no peers before anyone connects");

    // Three peers on ONE listener, one poll thread, one transport_t.
    std::printf("three peers on one listener:\n");
    std::vector<std::unique_ptr<raw_peer_t>> clients;
    for (int i = 0; i < 3; ++i)
        clients.push_back(std::make_unique<raw_peer_t>(server.local_port()));
    check(ok, clients[0]->ok() && clients[1]->ok() && clients[2]->ok(), "all three connected");
    check(ok, wait_for_peers(facet, 3, 2s), "the listener accepted all three into slots");

    const auto names = peers_of(facet);
    check(ok, names.size() == 3, "…and lists exactly three peers");
    check(ok, names == std::vector<std::string>{"p0", "p1", "p2"},
          "…named p0/p1/p2 — by SLOT, which is a position and not an identity");

    // A DIRECTED send: one named peer's socket, and nobody else's.
    std::printf("a directed send:\n");
    const auto only_for_p1 = frame_of(6, 0x10);
    tr::net::transport_t* to_p1 = facet.peer_link("p1");
    check(ok, to_p1 != nullptr, "peer_link('p1') resolved a directed endpoint");
    if (to_p1 != nullptr) to_p1->send(only_for_p1);
    const auto want = record(only_for_p1);
    check(ok, clients[1]->read_within(want.size(), 2s) == want,
          "the addressed peer got the record, prefix and all");
    check(ok, clients[0]->read_within(1, 200ms).empty(), "…and peer p0 got nothing");
    check(ok, clients[2]->read_within(1, 200ms).empty(), "…and peer p2 got nothing");

    // The FLAT surface of the same object: `send` on the server itself fans out to every open
    // peer. One link, two addressing surfaces — the flat one for "this link", the facet for
    // "that peer on this link".
    std::printf("the flat broadcast on the same object:\n");
    const auto for_everyone = frame_of(4, 0x20);
    server.send(for_everyone);
    const auto expect = record(for_everyone);
    check(ok, clients[0]->read_within(expect.size(), 2s) == expect, "p0 got the broadcast");
    check(ok, clients[1]->read_within(expect.size(), 2s) == expect, "p1 got it too");
    check(ok, clients[2]->read_within(expect.size(), 2s) == expect, "and so did p2");

    // A name outside the live slot set resolves to nothing — the endpoint is not synthesized
    // on demand, so a stale name cannot be sent to.
    check(ok, facet.peer_link("p9") == nullptr, "a name no slot holds resolves to no endpoint");

    check(ok, server.dropped_tx() == 0, "nothing was shed on the way out");

    std::printf("one listener, %zu peers in slots, cap %zu, 1 directed + 1 broadcast\n",
                names.size(), server.max_peers());
    return ok ? 0 : 1;
}
