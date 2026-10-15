/**
 * @file
 * @brief The per-link base RAM ratchet (#2156): what an IDLE stream server draws from its
 *        injected memory, before any peer connects, pinned so growth fails the build's tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_conn_ram`'s `link_base` row priced this, but it ran on one host in warn mode and,
 * once the transports drew from the allocation seam (#1780), it counted `operator new` only,
 * which the default slab root never reaches. The base grew from 464 B to 2608 B while it
 * could not see the draws: a dead 8-slot send ring inherited by every server (#1629), and a
 * poll set sized up front to the whole admission cap (#1780). This test counts the link's
 * own seam instead (`link_memory_t::io` and `state`), on every core-ci leg, so the same
 * growth now fails a test rather than a warning nobody reads.
 *
 * The pins are RATCHETS with no headroom, like `kMaxVertexBytes64`: LOWER one in the commit
 * that shrinks the link; RAISING one is a reviewed decision that names the bytes it buys.
 * They are 64-bit numbers (block sizes plus the 8-entry first block of a `block_array_t`);
 * on another pointer width the test reports and does not gate.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_ws.hpp"

namespace {

using namespace std::chrono_literals;

int g_failures = 0;

/** @brief Record one check, printing it either way. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief A forwarding source over the platform heap that keeps the live byte balance. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("link_base_ram") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr) live.fetch_add(bytes, std::memory_order_relaxed);
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        live.fetch_sub(bytes, std::memory_order_relaxed);
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::atomic<std::size_t> live{0}; /**< @brief Bytes drawn and not yet released. */
};

/** @brief Pinned idle draw of a stream server on a 64-bit target: the poll set's first block
 *         (8 `pollfd`s for the listen socket). Was 1992 B before #2156. */
constexpr std::size_t kServerBaseBytes64 = 64;

/**
 * @brief Construct one idle server on a counting seam and compare its draw to the pin.
 *
 * @tparam Server The stream server under test (`tcp_server_transport_t`, `ws_server_transport_t`).
 * @tparam Config Its config aggregate.
 */
template <class Server, class Config>
void run_arm(const char* name) {
    counting_source_t src;
    std::size_t base = 0;
    {
        Config cfg{};
        cfg.memory.io = &src;
        cfg.memory.state = &src;
        Server server(0, cfg);
        check(server.ok(), "server bound");
        // The poll thread draws its set on its first pass; give it time to run that pass.
        std::this_thread::sleep_for(300ms);
        base = src.live.load(std::memory_order_relaxed);
    }
    std::printf("%s: idle draw %zu B (sizeof %zu B)\n", name, base, sizeof(Server));
    if constexpr (sizeof(void*) == 8) {
        check(base <= kServerBaseBytes64, "idle draw within the 64-bit pin");
    }
    check(src.live.load(std::memory_order_relaxed) == 0, "teardown returned every byte");
}

}  // namespace

int main() {
    std::printf("link_base_ram_test (#2156):\n");
    run_arm<tr::net::tcp_server_transport_t, tr::net::tcp_server_config_t>("tcp-server");
    run_arm<tr::net::ws_server_transport_t, tr::net::ws_server_config_t>("ws-server");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all checks passed\n");
    return EXIT_SUCCESS;
}
