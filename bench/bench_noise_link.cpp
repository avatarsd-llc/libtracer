/**
 * @file
 * @brief The Noise link harness's LINK arms on libtracer (#2065): the plain UDP link now, and
 *        the Noise link (#2064) behind one switch, with the same rows.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Both arms run through `link_harness.hpp`, the driver `bench_zenoh_link` runs Zenoh's links
 * through, so libtracer and Zenoh, plain and secured, are measured by one piece of code: one
 * warm session per arm, a request/reply row and a streaming row at 64 B, 1 KiB, 4 KiB, 16 KiB
 * and the datagram bound, a session-setup row (the handshake, for a secured link) and the RAM
 * one pair holds.
 *
 *  - **`udp`** — two `udp_transport_t` on loopback: the server end binds an ephemeral port and
 *    learns its peer from the first datagram; the client end dials it. The baseline the Noise
 *    link is judged against.
 *  - **`noise`** — compiled only with `-DLIBTRACER_BENCH_NOISE_LINK=ON`, which also needs the
 *    link kind from #2064 in the tree. `make_noise_pair` is the one place that names that link;
 *    it is written against the shape #2064's issue describes (a UDP-carried link opened with a
 *    pre-shared key, driven by the application's `now`), and #2064 adjusts its few lines to the
 *    constructor it actually ships. Every row, label and reading is shared with `udp`.
 *
 * The crypto floor under the Noise arm (seal/open per frame, the handshake on its own) is
 * `bench_noise_crypto`. Diagnostic, not a gate.
 *
 *     bench_noise_link                 # every compiled arm
 *     bench_noise_link --arm=udp       # one arm
 *     bench_noise_link --quick         # a fifth of the time budget (a smoke run)
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/batch.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport_udp.hpp"
#include "link_harness.hpp"

#if defined(LIBTRACER_BENCH_NOISE_LINK)
#include "libtracer/transport_noise.hpp"  // #2064
#endif

namespace {

using bench::link::link_pair_t;
using bench::link::rx_state_t;

/**
 * @brief The server end's sink: report, and echo what the harness says to echo. On a
 *        `stream-batch` row each datagram is one batch record, and every sample in it is
 *        reported as the frame it carries.
 */
struct server_sink_t {
    rx_state_t* rx = nullptr;            /**< @brief Where frames are reported. */
    tr::net::transport_t* end = nullptr; /**< @brief The server end, for the echo. */
    /** @brief The borrowed-span receiver callable. */
    void operator()(std::span<const std::byte> f) const {
        if (!rx->batch.load(std::memory_order_relaxed)) {
            if (rx->on_server(f)) end->send(f);
            return;
        }
        const auto node = tr::wire::tlv_node_t::over(f);
        if (!node) return;
        const auto b = tr::wire::read_batch(*node, /*dt_ns=*/1);
        if (!b) return;
        for (const tr::wire::tlv_node_t sample : b->samples) (void)rx->on_server(sample.payload());
    }
};

/** @brief The client end's sink: report the echo. */
struct client_sink_t {
    rx_state_t* rx = nullptr; /**< @brief Where frames are reported. */
    /** @brief The borrowed-span receiver callable. */
    void operator()(std::span<const std::byte> f) const { rx->on_client(f); }
};

/** @brief Two libtracer link ends of one kind, wired to the harness. */
template <class T>
class pair_t final : public link_pair_t {
   public:
    /** @brief Take the two constructed ends and install their sinks. */
    pair_t(rx_state_t& rx, std::unique_ptr<T> server, std::unique_ptr<T> client)
        : server_(std::move(server)), client_(std::move(client)) {
        server_sink_ = {&rx, server_.get()};
        client_sink_ = {&rx};
        server_->set_receiver(server_sink_);
        client_->set_receiver(client_sink_);
    }
    ~pair_t() override {
        client_.reset();  // the client first, so the server never echoes into a dead socket
        server_.reset();
    }
    void send(std::span<const std::byte> f) override { client_->send(f); }

    std::vector<std::span<std::byte>> stage_batch(std::size_t k, std::size_t v) override {
        frames_.assign(k, {});
        views_.clear();
        std::vector<std::span<std::byte>> out;
        const std::vector<std::byte> body(v, std::byte{0x5A});
        for (std::vector<std::byte>& f : frames_) {
            tr::wire::emit_tlv(f, tr::wire::type_t::VALUE, tr::wire::opt_t{}, body);
            views_.push_back(
                tr::view::view_t::over(tr::view::borrow_const(std::span<const std::byte>(f))));
            out.push_back(std::span<std::byte>(f).subspan(f.size() - v));
        }
        return out;
    }

    /** @brief What an application does: rope the K samples into one value, send it once. */
    void send_batch() override {
        const tr::view::rope_t r = tr::wire::compose_batch(
            tr::mem::heap_backend(), tr::wire::batch_carriage_t::STANDALONE, 1, views_);
        iov_.clear();
        r.walk([this](std::span<const std::byte> s) { iov_.push_back(s); });
        client_->send(std::span<const std::span<const std::byte>>(iov_));
    }

#if defined(LIBTRACER_BENCH_NOISE_LINK)
    void tick(std::uint64_t now) override {
        if constexpr (requires(T& t) { t.tick(now); }) {
            server_->tick(now);
            client_->tick(now);
        }
    }
#endif

   private:
    std::unique_ptr<T> server_;
    std::unique_ptr<T> client_;
    server_sink_t server_sink_;
    client_sink_t client_sink_;
    std::vector<std::vector<std::byte>> frames_;   // the staged sample frames
    std::vector<tr::view::view_t> views_;          // borrowed views over frames_
    std::vector<std::span<const std::byte>> iov_;  // the composed rope's links
};

/**
 * @brief A UDP pair: the server end binds an ephemeral port and learns its peer; the client
 *        end dials it. No frame flows until the harness sends one, by which time `pair_t` has
 *        installed both sinks.
 */
[[nodiscard]] std::unique_ptr<link_pair_t> make_udp_pair(rx_state_t& rx) {
    auto server = std::make_unique<tr::net::udp_transport_t>(0, "", 0);
    if (!server->ok()) return nullptr;
    auto client = std::make_unique<tr::net::udp_transport_t>(0, "127.0.0.1", server->local_port());
    if (!client->ok()) return nullptr;
    return std::make_unique<pair_t<tr::net::udp_transport_t>>(rx, std::move(server),
                                                              std::move(client));
}

#if defined(LIBTRACER_BENCH_NOISE_LINK)
/** @brief The PSK both Noise ends share. */
constexpr std::array<std::byte, 32> kPsk = [] {
    std::array<std::byte, 32> k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::byte>(0x20 + i);
    return k;
}();

/**
 * @brief A Noise pair — the ONLY code that names #2064's link kind. Written against the shape
 *        #2064 describes: the UDP carrier's (bind port, peer host, peer port) plus a PSK, with
 *        the application driving `tick(now)`; adjust these lines to the constructor it ships.
 */
[[nodiscard]] std::unique_ptr<link_pair_t> make_noise_pair(rx_state_t& rx) {
    using noise_t = tr::net::noise_transport_t;
    auto server = std::make_unique<noise_t>(0, "", 0, tr::net::noise_config_t{.psk = kPsk});
    if (!server->ok()) return nullptr;
    auto client = std::make_unique<noise_t>(0, "127.0.0.1", server->local_port(),
                                            tr::net::noise_config_t{.psk = kPsk});
    if (!client->ok()) return nullptr;
    return std::make_unique<pair_t<noise_t>>(rx, std::move(server), std::move(client));
}
#endif

}  // namespace

int main(int argc, char** argv) {
    bench::link::options_t o;
    if (!bench::link::parse_options(argc, argv, o)) return 2;
    std::printf("# bench_noise_link: libtracer link arms (#2065), one warm pair per arm\n");
    if (!o.list) bench::emit_clock_floor();
    const bench::link::arm_t arms[] = {
        {"libtracer", "udp", bench::link::kDatagramBound, "link-unbatched", make_udp_pair},
#if defined(LIBTRACER_BENCH_NOISE_LINK)
        {"libtracer", "noise", bench::link::kDatagramBound - 16, "link-unbatched", make_noise_pair},
#endif
    };
#if !defined(LIBTRACER_BENCH_NOISE_LINK)
    std::printf("# arm libtracer/noise: compiled out (-DLIBTRACER_BENCH_NOISE_LINK=ON, #2064)\n");
#endif
    bool ok = true;
    for (const auto& a : arms) ok &= bench::link::run_arm(a, o);
    return ok ? 0 : 1;
}
