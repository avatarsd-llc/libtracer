/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #55 (increment 2) — transport_can SocketCAN-binding tests over an in-memory
 * fake link, so they pass in plain Docker with NO kernel CAN (vcan). A
 * fake_can_bus_t wires two (or three) fake_link_t endpoints; each endpoint drains
 * its inbound queue on a worker thread, so the transport's receive path is genuinely
 * cross-thread (TSan-meaningful), mirroring socketcan_link_t's real recv thread.
 *
 * Coverage:
 *   - multi-CAN-frame TLV round-trips byte-exact, both CLASSIC and CAN-FD;
 *   - single-frame TLV round-trips (the lean-value path);
 *   - the dynamic identity↔path map learns the in-band advertise binding;
 *   - CAN-FD egress DLC padding is correct (tail window padded up the DLC lattice)
 *     yet the delivered frame is trimmed byte-exact;
 *   - lifecycle is clean (construct → send → destruct, no leaks/races).
 *
 * The real-vcan end-to-end smoke test lives in the can-vcan-e2e CI job.
 */

#include "libtracer/transport_can.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/can.hpp"
#include "libtracer/view_can.hpp"

namespace {

using namespace std::chrono_literals;
namespace can = tr::net::can;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// ---------------------------------------------------------------------------
// In-memory fake CAN bus + link (the test seam impl of can_link_t).
// ---------------------------------------------------------------------------

class fake_link_t;

class fake_can_bus_t {
   public:
    void attach(fake_link_t* l) {
        const std::lock_guard lock(m_);
        links_.push_back(l);
    }
    void detach(fake_link_t* l) {
        const std::lock_guard lock(m_);
        for (auto it = links_.begin(); it != links_.end(); ++it) {
            if (*it == l) {
                links_.erase(it);
                break;
            }
        }
    }
    void broadcast(fake_link_t* from, const tr::net::can_frame_data_t& f);

   private:
    std::mutex m_;
    std::vector<fake_link_t*> links_;
};

class fake_link_t : public tr::net::can_link_t {
   public:
    explicit fake_link_t(fake_can_bus_t& bus) : bus_(bus) {
        bus_.attach(this);
        worker_ = std::thread([this] { run(); });
    }
    ~fake_link_t() override {
        bus_.detach(this);
        {
            const std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    fake_link_t(const fake_link_t&) = delete;
    fake_link_t& operator=(const fake_link_t&) = delete;

    void write_raw(const tr::net::can_frame_data_t& f) override { bus_.broadcast(this, f); }
    void on_receive(rx_fn_t rx) override {
        const std::lock_guard lock(m_);
        rx_ = std::move(rx);
    }
    void enqueue(const tr::net::can_frame_data_t& f) {
        const std::lock_guard lock(m_);
        q_.push_back(f);
        cv_.notify_one();
    }

   private:
    void run() {
        std::unique_lock lock(m_);
        while (true) {
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) return;
            tr::net::can_frame_data_t f = q_.front();
            q_.pop_front();
            rx_fn_t rx = rx_;
            lock.unlock();
            if (rx) rx(f);
            lock.lock();
        }
    }

    fake_can_bus_t& bus_;
    rx_fn_t rx_;
    std::deque<tr::net::can_frame_data_t> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread worker_;
};

void fake_can_bus_t::broadcast(fake_link_t* from, const tr::net::can_frame_data_t& f) {
    const std::lock_guard lock(m_);
    for (auto* l : links_) {
        if (l != from) l->enqueue(f);
    }
}

// A passive endpoint that records every frame it sees on the bus (a sniffer).
class frame_tap_t {
   public:
    explicit frame_tap_t(fake_can_bus_t& bus) : link_(bus) {
        link_.on_receive([this](const tr::net::can_frame_data_t& f) {
            const std::lock_guard lock(m_);
            frames_.push_back(f);
        });
    }
    [[nodiscard]] std::vector<tr::net::can_frame_data_t> snapshot() {
        const std::lock_guard lock(m_);
        return frames_;
    }
    // Poll until at least `n` frames have been recorded (the tap drains on its own
    // worker thread, so it may lag the data path that already delivered).
    void wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                const std::lock_guard lock(m_);
                if (frames_.size() >= n) return;
            }
            std::this_thread::sleep_for(5ms);
        }
    }

   private:
    fake_link_t link_;
    std::mutex m_;
    std::vector<tr::net::can_frame_data_t> frames_;
};

// A receiver sink that captures delivered frames for the test thread.
class sink_t {
   public:
    void on(std::span<const std::byte> f) {
        const std::lock_guard lock(m_);
        last_.assign(f.begin(), f.end());
        ++count_;
        cv_.notify_all();
    }
    // Wait until at least `n` frames have been delivered (or the deadline passes).
    bool wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [&] { return count_ >= n; });
    }
    [[nodiscard]] std::vector<std::byte> last() {
        const std::lock_guard lock(m_);
        return last_;
    }

   private:
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::byte> last_;
    std::size_t count_ = 0;
};

std::vector<std::byte> make_payload(std::size_t n) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>((i * 7 + 3) & 0xFF);
    return v;
}

bool equal_bytes(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

void test_roundtrip(tr::view::can_frame_mode_t mode, std::size_t payload_len, const char* label) {
    std::printf("transport_can round trip (%s, %zu bytes):\n", label, payload_len);

    fake_can_bus_t bus;
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can tx_a(std::move(link_a), {0, 1, mode, "sensor/temp"});
    tr::net::transport_can tx_b(std::move(link_b), {0, 2, mode, "actuator/valve"});

    sink_t sink;
    tx_b.set_receiver([&](std::span<const std::byte> f) { sink.on(f); });

    const std::vector<std::byte> payload = make_payload(payload_len);
    tx_a.send(payload);

    const bool got = sink.wait_for_count(1, 2s);
    check(got, "frame delivered to peer receiver");
    if (got) check(equal_bytes(sink.last(), payload), "delivered bytes are byte-exact");

    // The peer learned the in-band advertise binding for A's first group (base
    // endpoint 1, node 1, version 0).
    const std::uint32_t base_id = can::encode_can_id({0, 1, tr::net::kCanFirstDataEndpoint});
    const auto binding = tx_b.learned_binding(base_id);
    check(binding.has_value(), "identity↔path map learned the advertise binding");
    if (binding) {
        check(binding->path == "sensor/temp", "learned binding carries the advertised path");
        check(binding->group_total_len == payload_len, "learned binding total length matches");
    }
}

void test_fd_dlc_padding() {
    std::printf("transport_can CAN-FD DLC padding:\n");

    fake_can_bus_t bus;
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);
    frame_tap_t tap(bus);  // records every frame on the bus

    tr::net::transport_can tx_a(std::move(link_a), {0, 1, tr::view::can_frame_mode_t::FD, "p"});
    tr::net::transport_can tx_b(std::move(link_b), {0, 2, tr::view::can_frame_mode_t::FD, "q"});

    sink_t sink;
    tx_b.set_receiver([&](std::span<const std::byte> f) { sink.on(f); });

    // 100 bytes in FD: windows 64 + 36; the 36-byte tail pads up to DLC 48.
    const std::vector<std::byte> payload = make_payload(100);
    tx_a.send(payload);
    check(sink.wait_for_count(1, 2s), "FD payload delivered");
    check(equal_bytes(sink.last(), payload), "FD delivered bytes byte-exact (padding trimmed)");

    // The tap drains on its own thread and lags the delivery path; the frame
    // census (hellos + advertise + data) is a v2 wire detail the test should not
    // hard-code — wait for the condition itself: the padded tail data frame.
    bool saw_full = false, saw_padded = false, saw_control = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        saw_full = saw_padded = saw_control = false;
        for (const auto& f : tap.snapshot()) {
            const auto fields = can::decode_can_id(f.id);
            if (!fields) continue;
            if (fields->endpoint == tr::net::kCanControlEndpoint) {
                saw_control = true;
                continue;
            }
            if (f.len == 64) saw_full = true;
            if (f.len == 48) saw_padded = true;  // can_fd_dlc_round_up(36) == 48
        }
        if (saw_control && saw_full && saw_padded) break;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    check(saw_control, "an advertise rode the control slot");
    check(saw_full, "interior FD slice carried a full 64-byte data field");
    check(saw_padded, "tail FD slice padded up to DLC 48 (can_fd_dlc_round_up(36))");
}

void test_control_stream_resync() {
    std::printf("transport_can control-stream resync (mid-stream join):\n");

    fake_can_bus_t bus;
    auto link_b = std::make_unique<fake_link_t>(bus);
    fake_link_t injector(bus);  // stands in for the tail of an in-flight advertise

    // B is listening BEFORE A exists — then a fragment of node 1's control
    // stream arrives (what a mid-stream join or a lost control frame leaves
    // behind). Without resynchronization this wedges B's decoder for node 1
    // permanently and A's later traffic is never delivered.
    tr::net::transport_can tx_b(std::move(link_b),
                                {0, 2, tr::view::can_frame_mode_t::CLASSIC, "q"});
    sink_t sink;
    tx_b.set_receiver([&](std::span<const std::byte> f) { sink.on(f); });

    tr::net::can_frame_data_t fragment;
    fragment.id = can::encode_can_id({0, 1, tr::net::kCanControlEndpoint});
    fragment.fd = false;
    fragment.len = 5;
    const std::uint8_t tail[5] = {0x6F, 0x72, 0x2F, 0x74, 0x65};  // "or/te" — path bytes
    for (std::size_t i = 0; i < 5; ++i) fragment.data[i] = static_cast<std::byte>(tail[i]);
    injector.write_raw(fragment);

    auto link_a = std::make_unique<fake_link_t>(bus);
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "sensor/temp"});
    const std::vector<std::byte> payload = make_payload(24);
    tx_a.send(payload);

    const bool got = sink.wait_for_count(1, 2s);
    check(got, "delivery survives a garbage control-stream prefix (resync)");
    if (got) check(equal_bytes(sink.last(), payload), "resynced delivery is byte-exact");
}

void test_lifecycle() {
    std::printf("transport_can lifecycle (construct/send/destruct):\n");
    fake_can_bus_t bus;
    for (int i = 0; i < 8; ++i) {
        auto link_a = std::make_unique<fake_link_t>(bus);
        auto link_b = std::make_unique<fake_link_t>(bus);
        tr::net::transport_can tx_a(std::move(link_a),
                                    {0, 1, tr::view::can_frame_mode_t::CLASSIC, "a"});
        tr::net::transport_can tx_b(std::move(link_b),
                                    {0, 2, tr::view::can_frame_mode_t::CLASSIC, "b"});
        sink_t sink;
        tx_b.set_receiver([&](std::span<const std::byte> f) { sink.on(f); });
        tx_a.send(make_payload(30));
        sink.wait_for_count(1, 1s);
    }
    check(true, "8 construct/send/destruct cycles completed without crash");
}

// A single-frame value should also self-deliver via its advertise (slice_count 1).
void test_single_value() {
    std::printf("transport_can single value (one frame):\n");
    fake_can_bus_t bus;
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "p"});
    tr::net::transport_can tx_b(std::move(link_b),
                                {0, 2, tr::view::can_frame_mode_t::CLASSIC, "q"});
    sink_t sink;
    tx_b.set_receiver([&](std::span<const std::byte> f) { sink.on(f); });
    const std::vector<std::byte> payload = make_payload(5);
    tx_a.send(payload);
    check(sink.wait_for_count(1, 2s), "single-frame value delivered");
    check(equal_bytes(sink.last(), payload), "single-frame bytes byte-exact");
}

}  // namespace

int main() {
    test_roundtrip(tr::view::can_frame_mode_t::CLASSIC, 20, "classic, multi-frame");
    test_roundtrip(tr::view::can_frame_mode_t::FD, 150, "CAN-FD, multi-frame");
    test_single_value();
    test_fd_dlc_padding();
    test_control_stream_resync();
    test_lifecycle();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
