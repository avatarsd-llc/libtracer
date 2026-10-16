/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief CAN send + reassembly CPU per frame size, and the receiver's RX-store RAM (#2049).
 *
 * Two production `can_transport_t` ends on an in-process bus. Each sample is one `send()` on
 * node 1 and the delivery of every frame it emitted to node 2, which reassembles and delivers
 * it: the sender's segmenting, the receiver's advertise learning, and its reassembly, all on
 * this thread. CPU is the thread's own CPU clock, so a busy host stretches wall time, not this.
 *
 * Rows: classic and FD at 4 B (the small send), 64 B, 1 KiB, 4 KiB, 16 KiB and 64 KiB. A size
 * whose slice count no group can hold (64 KiB classic, over 4095 slices) is refused by the
 * sender and reported as such. Each row is the BEST of `rounds` rounds of `n` sends: noise on a
 * shared host only ever adds time, so the best round is the one closest to the code.
 *
 * The receiver's RX tables draw from a counting source, so `rx_live_B` is what the link holds
 * at steady state after the warm-up (learned bindings, per-node state, no group in flight) and
 * `rx_peak_B` is the most it held during the row.
 *
 * Output: one `CANREASM` line per row, `key=value` pairs. `bench_can_reassembly [quick] [payload]`:
 * `quick` is a one-round smoke pass, and a payload size runs that row only.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/tracer.hpp"
#include "libtracer/transport_can.hpp"

namespace {

/** @brief A block source that keeps a live and a peak byte balance over an inner source. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    explicit counting_source_t(tr::mem::block_source_t& inner) noexcept
        : block_source_t("counting"), inner_(&inner) {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* p = inner_->try_alloc(bytes, align);
        if (p != nullptr) {
            live += static_cast<long long>(bytes);
            peak = std::max(peak, live);
        }
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        if (p != nullptr) live -= static_cast<long long>(bytes);
        inner_->release(p, bytes, align);
    }
    long long live = 0; /**< @brief Bytes currently held. */
    long long peak = 0; /**< @brief Most bytes held since the last reset. */

   private:
    tr::mem::block_source_t* inner_;
};

class can_end_t;
using can_queue_t = std::deque<std::pair<can_end_t*, tr::net::can_frame_data_t>>;

/** @brief One end of the in-process bus; frames are queued and delivered by `pump`. */
class can_end_t final : public tr::net::can_link_t {
   public:
    explicit can_end_t(can_queue_t& q) : q_(q) {}
    can_end_t* peer = nullptr;
    void write_raw(const tr::net::can_frame_data_t& f) override {
        if (peer != nullptr) q_.emplace_back(peer, f);
    }
    void on_receive(rx_fn_t rx) override { rx_ = std::move(rx); }
    void start() override {}
    void deliver(const tr::net::can_frame_data_t& f) {
        if (rx_) rx_(f);
    }

   private:
    can_queue_t& q_;
    rx_fn_t rx_;
};

/** @brief This thread's CPU time, in nanoseconds. */
[[nodiscard]] std::uint64_t thread_cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

/** @brief A node-1 → node-2 pair on one in-process bus, the receiver on a counting store. */
struct pair_t {
    can_queue_t q;
    counting_source_t rx_store{tr::mem::net_source()};
    can_end_t* a_end = nullptr;
    can_end_t* b_end = nullptr;
    std::unique_ptr<tr::net::can_transport_t> a, b;
    std::size_t delivered = 0;

    explicit pair_t(bool fd) {
        const auto mk = [&](std::uint16_t node, can_end_t*& out, tr::mem::block_source_t* rx) {
            auto link = tr::mem::make_poly<can_end_t>(tr::mem::net_source(), q);
            out = link.get();
            tr::net::transport_can_config_t cfg;
            cfg.node = node;
            cfg.mode =
                fd ? tr::net::can::can_frame_mode_t::FD : tr::net::can::can_frame_mode_t::CLASSIC;
            cfg.path = "s";
            cfg.reasm_src = rx;
            return std::make_unique<tr::net::can_transport_t>(std::move(link), cfg);
        };
        a = mk(1, a_end, nullptr);
        b = mk(2, b_end, &rx_store);
        a_end->peer = b_end;
        b_end->peer = a_end;
        b->set_receiver(
            [](void* ctx, std::span<const std::byte>) { ++static_cast<pair_t*>(ctx)->delivered; },
            this);
        pump();
    }
    void pump() {
        while (!q.empty()) {
            auto [to, f] = q.front();
            q.pop_front();
            to->deliver(f);
        }
    }
};

/** @brief Run one row and print its `CANREASM` line. */
void run_row(bool fd, std::size_t payload, std::size_t n, std::size_t rounds) {
    pair_t p(fd);
    std::vector<std::byte> frame(payload);
    for (std::size_t i = 0; i < payload; ++i) frame[i] = static_cast<std::byte>(i * 31u + 7u);

    // Warm-up: a whole lap of the receiver's endpoint window, so its binding table is at the
    // steady state a long-lived link reaches (the table only shrinks when a run is re-issued).
    const std::uint64_t refused0 = p.a->dropped_tx();
    const std::size_t warm = std::max<std::size_t>(n, 4096);
    for (std::size_t i = 0; i < warm; ++i) {
        p.a->send(frame);
        p.pump();
    }
    const bool refused = p.a->dropped_tx() != refused0;
    p.rx_store.peak = p.rx_store.live;
    const long long rx_live = p.rx_store.live;

    std::uint64_t best = ~0ull;
    p.delivered = 0;
    for (std::size_t r = 0; r < rounds; ++r) {
        const std::uint64_t t0 = thread_cpu_ns();
        for (std::size_t i = 0; i < n; ++i) {
            p.a->send(frame);
            p.pump();
        }
        best = std::min(best, (thread_cpu_ns() - t0) / n);
    }
    std::printf(
        "CANREASM bus=%s payload=%zu n=%zu rounds=%zu status=%s cpu_ns_per_send=%llu "
        "delivered=%zu rx_live_B=%lld rx_peak_B=%lld\n",
        fd ? "fd" : "classic", payload, n, rounds, refused ? "refused" : "ok",
        static_cast<unsigned long long>(best), p.delivered, rx_live, p.rx_store.peak);
}

}  // namespace

int main(int argc, char** argv) {
    // `quick` runs a short pass for CI smoke; the default is the measurement.
    const bool quick = argc > 1 && std::string_view(argv[1]) == "quick";
    // An optional second argument runs that one payload size only (for a profiler).
    const std::size_t only = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 0;
    const std::size_t rounds = quick ? 1 : 5;
    for (const bool fd : {false, true}) {
        for (const std::size_t payload : {4u, 64u, 1024u, 4096u, 16384u, 65536u}) {
            if (only != 0 && payload != only) continue;
            const std::size_t n = quick ? 4
                                        : (payload >= 16384  ? 20
                                           : payload >= 4096 ? 100
                                                             : 2000);
            run_row(fd, payload, n, rounds);
        }
    }
    return 0;
}
