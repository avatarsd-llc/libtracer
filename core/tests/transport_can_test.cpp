/**
 * @file
 * @brief #55 (increment 2) — transport_can SocketCAN-binding tests over an in-memory fake link, so
 *        they pass in plain Docker with NO kernel CAN (vcan).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A
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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/can.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_pool.hpp"
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

/** @brief A passive endpoint that records every frame it sees on the bus (a sniffer). */
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
    /**
     * @brief Poll until at least `n` frames have been recorded (the tap drains on its own worker
     *        thread, so it may lag the data path that already delivered).
     */
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

/** @brief A receiver sink that captures delivered frames for the test thread. */
class sink_t {
   public:
    void on(std::span<const std::byte> f) {
        const std::lock_guard lock(m_);
        last_.assign(f.begin(), f.end());
        ++count_;
        cv_.notify_all();
    }
    /** @brief Wait until at least `n` frames have been delivered (or the deadline passes). */
    bool wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [&] { return count_ >= n; });
    }
    [[nodiscard]] std::vector<std::byte> last() {
        const std::lock_guard lock(m_);
        return last_;
    }
    /** @brief How many frames have been delivered so far (an EXACT count, not a floor). */
    [[nodiscard]] std::size_t count() {
        const std::lock_guard lock(m_);
        return count_;
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

/** @brief Poll `cond` until it holds or `budget` elapses (the RX path runs off-thread). */
template <class Fn>
bool wait_until(Fn cond, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return cond();
}

/**
 * @brief Emit `adv` as node `node`'s control-slot byte stream, in CLASSIC ≤8B windows —
 *        exactly what `transport_can::emit_advertise` puts on the wire.
 */
void inject_advertise(fake_link_t& link, std::uint16_t node, const can::advertise_t& adv) {
    const std::vector<std::byte> bytes = can::encode_advertise(adv);
    const std::uint32_t control_id = can::encode_can_id({0, node, tr::net::kCanControlEndpoint});
    for (std::size_t off = 0; off < bytes.size(); off += 8) {
        tr::net::can_frame_data_t f;
        f.id = control_id;
        f.fd = false;
        const std::size_t n = std::min<std::size_t>(8, bytes.size() - off);
        std::memcpy(f.data.data(), bytes.data() + off, n);
        f.len = static_cast<std::uint8_t>(n);
        link.write_raw(f);
    }
}

/** @brief Emit one raw 8-byte data slice from node `node` on `endpoint`. */
void inject_data(fake_link_t& link, std::uint16_t node, std::uint16_t endpoint) {
    tr::net::can_frame_data_t f;
    f.id = can::encode_can_id({0, node, endpoint});
    f.fd = false;
    f.len = 8;
    for (std::size_t i = 0; i < 8; ++i) f.data[i] = static_cast<std::byte>(endpoint + i);
    link.write_raw(f);
}

/** @brief The shared shape of the RX-bounding tests: node 2 listening, node 1 injecting raw. */
tr::net::transport_can_config_t rx_test_config() {
    tr::net::transport_can_config_t cfg;
    cfg.version = 0;
    cfg.node = 2;
    cfg.mode = tr::view::can_frame_mode_t::CLASSIC;
    cfg.path = "q";
    return cfg;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

void test_roundtrip(tr::view::can_frame_mode_t mode, std::size_t payload_len, const char* label) {
    std::printf("transport_can round trip (%s, %zu bytes):\n", label, payload_len);

    fake_can_bus_t bus;
    // Sink + named receiver lambda BEFORE the transports: the slot binds the
    // callable by address, and ~transport_can joins its receive thread.
    sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can tx_a(std::move(link_a), {0, 1, mode, "sensor/temp"});
    tr::net::transport_can tx_b(std::move(link_b), {0, 2, mode, "actuator/valve"});

    tx_b.set_receiver(rx);

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
    sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);
    frame_tap_t tap(bus);  // records every frame on the bus

    tr::net::transport_can tx_a(std::move(link_a), {0, 1, tr::view::can_frame_mode_t::FD, "p"});
    tr::net::transport_can tx_b(std::move(link_b), {0, 2, tr::view::can_frame_mode_t::FD, "q"});

    tx_b.set_receiver(rx);

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
    sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
    auto link_b = std::make_unique<fake_link_t>(bus);
    fake_link_t injector(bus);  // stands in for the tail of an in-flight advertise

    // B is listening BEFORE A exists — then a fragment of node 1's control
    // stream arrives (what a mid-stream join or a lost control frame leaves
    // behind). Without resynchronization this wedges B's decoder for node 1
    // permanently and A's later traffic is never delivered.
    tr::net::transport_can tx_b(std::move(link_b),
                                {0, 2, tr::view::can_frame_mode_t::CLASSIC, "q"});
    tx_b.set_receiver(rx);

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
        sink_t sink;
        auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
        auto link_a = std::make_unique<fake_link_t>(bus);
        auto link_b = std::make_unique<fake_link_t>(bus);
        tr::net::transport_can tx_a(std::move(link_a),
                                    {0, 1, tr::view::can_frame_mode_t::CLASSIC, "a"});
        tr::net::transport_can tx_b(std::move(link_b),
                                    {0, 2, tr::view::can_frame_mode_t::CLASSIC, "b"});
        tx_b.set_receiver(rx);
        tx_a.send(make_payload(30));
        sink.wait_for_count(1, 1s);
    }
    check(true, "8 construct/send/destruct cycles completed without crash");
}

/** @brief A single-frame value should also self-deliver via its advertise (slice_count 1). */
void test_single_value() {
    std::printf("transport_can single value (one frame):\n");
    fake_can_bus_t bus;
    sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "p"});
    tr::net::transport_can tx_b(std::move(link_b),
                                {0, 2, tr::view::can_frame_mode_t::CLASSIC, "q"});
    tx_b.set_receiver(rx);
    const std::vector<std::byte> payload = make_payload(5);
    tx_a.send(payload);
    check(sink.wait_for_count(1, 2s), "single-frame value delivered");
    check(equal_bytes(sink.last(), payload), "single-frame bytes byte-exact");
}

/**
 * @brief #912 — a flood of data slices on NEVER-ADVERTISED endpoints is capped by the
 *        injected `max_pending` and every eviction is counted.
 *
 * This is the reachable-by-any-bus-peer growth path: a data frame with no matching
 * binding is parked, and the only drain is `learn_advertise`'s covered-range re-drive,
 * which a peer that simply never advertises never triggers. `rx_ttl` is set far out of
 * reach here so the COUNT cap is unambiguously what bounds it.
 */
void test_pending_flood_is_capped_and_counted() {
    std::printf("transport_can pending-slice flood (never-advertised endpoints):\n");

    fake_can_bus_t bus;
    fake_link_t injector(bus);  // stands in for a peer that floods data, never advertises
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can_config_t cfg = rx_test_config();
    cfg.max_pending = 4;
    cfg.rx_ttl = 60s;  // far out of reach: the count cap must be what bounds this
    tr::net::transport_can tx_b(std::move(link_b), cfg);

    constexpr std::uint16_t kFlood = 64;
    for (std::uint16_t ep = 1; ep <= kFlood; ++ep) inject_data(injector, /*node=*/1, ep);

    const std::uint64_t expect_dropped = kFlood - cfg.max_pending;
    const bool settled = wait_until([&] { return tx_b.dropped_rx() >= expect_dropped; }, 2s);
    check(settled, "the flood's evictions were counted on dropped_rx()");
    check(tx_b.pending_slices() <= cfg.max_pending,
          "parked slices never exceed the injected max_pending");
    check(tx_b.dropped_rx() == expect_dropped,
          "every slice above the cap is counted exactly once (evict-and-count)");
    check(tx_b.dropped_groups() == 0, "no reassembly group was involved");
}

/**
 * @brief #912 — a parked slice whose advertise NEVER lands ages out.
 *
 * The count cap is disabled here, so the age-out is the only thing that can reclaim
 * these: this is the bound that holds under the shipped default config, where the
 * count caps are opt-in (RFC-0006 host-bounded).
 */
void test_pending_slices_age_out() {
    std::printf("transport_can pending-slice age-out (advertise never lands):\n");

    fake_can_bus_t bus;
    fake_link_t injector(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can_config_t cfg = rx_test_config();
    cfg.max_pending = 0;  // count cap OFF — only the age-out can reclaim these
    cfg.rx_ttl = 30ms;
    tr::net::transport_can tx_b(std::move(link_b), cfg);

    constexpr std::uint16_t kParked = 6;
    for (std::uint16_t ep = 1; ep <= kParked; ++ep) inject_data(injector, /*node=*/1, ep);
    check(wait_until([&] { return tx_b.pending_slices() == kParked; }, 2s),
          "all slices parked awaiting an advertise that never comes");

    std::this_thread::sleep_for(150ms);                   // >> rx_ttl
    inject_data(injector, /*node=*/1, /*endpoint=*/100);  // any inbound frame drives the sweep

    check(wait_until([&] { return tx_b.pending_slices() <= 1; }, 2s),
          "the stale parked slices were swept");
    check(tx_b.dropped_rx() >= kParked, "every aged-out slice is counted on dropped_rx()");
}

/**
 * @brief #912 — a degenerate `peer_ttl` of zero must not DISABLE the age-out.
 *
 * `rx_ttl` derives from `peer_ttl` when left at its `kCanRxTtlFromPeerTtl` sentinel,
 * so `peer_ttl_ms=0` derives `rx_ttl = 0`. Zero already means "instantly expired" to
 * the peer enumeration (`now - last_heard > 0`) and to the group sweep
 * (`sweep_stale(0)` retains only what was touched this instant) — but `expire_pending`
 * read the same zero as "sweep disabled" and returned early. With the shipped default
 * `max_pending = 0` (count cap off), that left the parked queue with NO bound at all:
 * exactly the unbounded growth #912 closes, re-opened by one config value.
 *
 * The two arms below are what make this non-vacuous. The zero arm must reclaim; the
 * far-window arm must NOT, or "the sweep ran" would be indistinguishable from "the
 * sweep runs unconditionally", which would be a different bug wearing this test's
 * green tick.
 */
void test_zero_peer_ttl_does_not_disable_the_age_out() {
    std::printf("transport_can zero peer_ttl keeps the age-out live (#912):\n");

    {
        fake_can_bus_t bus;
        fake_link_t injector(bus);  // a peer that floods data and never advertises
        auto link_b = std::make_unique<fake_link_t>(bus);

        tr::net::transport_can_config_t cfg = rx_test_config();
        cfg.max_pending = 0;  // count cap OFF — the age-out is the only bound
        cfg.peer_ttl = 0ms;   // degenerate: nothing is live
        cfg.rx_ttl = tr::net::kCanRxTtlFromPeerTtl;  // derive it — this is the vector
        tr::net::transport_can tx_b(std::move(link_b), cfg);

        // The claim is about GROWTH, so the flood has to be large enough that "it
        // did not grow" is not just "not much arrived yet".
        constexpr std::uint16_t kFlood = 64;
        for (std::uint16_t ep = 1; ep <= kFlood; ++ep) inject_data(injector, /*node=*/1, ep);

        check(wait_until([&] { return tx_b.dropped_rx() >= kFlood - 1; }, 2s),
              "a derived rx_ttl of 0 reclaims — it is not read as 'sweep disabled'");
        check(tx_b.pending_slices() <= 1,
              "the parked queue stays bounded with the count cap off (#912 does not re-open)");
    }

    {
        // The other arm: a window that has NOT elapsed must retain, or the assertion
        // above would pass for a sweep that simply drops everything every time.
        fake_can_bus_t bus;
        fake_link_t injector(bus);
        auto link_b = std::make_unique<fake_link_t>(bus);

        tr::net::transport_can_config_t cfg = rx_test_config();
        cfg.max_pending = 0;
        cfg.rx_ttl = 60s;  // far out of reach
        tr::net::transport_can tx_b(std::move(link_b), cfg);

        constexpr std::uint16_t kParked = 4;
        for (std::uint16_t ep = 1; ep <= kParked; ++ep) inject_data(injector, /*node=*/1, ep);
        check(wait_until([&] { return tx_b.pending_slices() == kParked; }, 2s),
              "slices park under a far window");
        inject_data(injector, /*node=*/1, /*endpoint=*/100);
        std::this_thread::sleep_for(50ms);
        check(tx_b.pending_slices() >= kParked, "a live window retains — the sweep is not blind");
        check(tx_b.dropped_rx() == 0, "nothing was counted as reclaimed");
    }
}

/**
 * @brief #912 — an incomplete reassembly group (a lost data slice) is erased by the
 *        on-advertise stale sweep and ticks dropped_groups().
 *
 * `erase` is reached only after `is_complete`, so before this a group missing any
 * slice pinned its buffered slices for the transport's life.
 */
void test_incomplete_group_is_swept() {
    std::printf("transport_can stale incomplete-group sweep (lost data slice):\n");

    fake_can_bus_t bus;
    fake_link_t injector(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can_config_t cfg = rx_test_config();
    cfg.rx_ttl = 30ms;
    tr::net::transport_can tx_b(std::move(link_b), cfg);

    // Node 1 advertises a 2-slice group at base endpoint 1 ...
    can::advertise_t adv;
    adv.can_id = can::encode_can_id({0, 1, tr::net::kCanFirstDataEndpoint});
    adv.group = true;
    adv.group_total_len = 16;
    adv.slice_count = 2;
    adv.path = "sensor/temp";
    inject_advertise(injector, /*node=*/1, adv);
    // ... and only the FIRST slice arrives; the second is lost on the bus.
    inject_data(injector, /*node=*/1, /*endpoint=*/tr::net::kCanFirstDataEndpoint);
    check(wait_until([&] { return tx_b.learned_binding(adv.can_id).has_value(); }, 2s),
          "the incomplete group's binding was learned");
    check(tx_b.dropped_groups() == 0, "nothing swept while the group is young");

    std::this_thread::sleep_for(150ms);  // >> rx_ttl

    // A later, unrelated advertise drives the sweep (every group is born of one).
    can::advertise_t later = adv;
    later.can_id = can::encode_can_id({0, 1, 10});
    later.group = false;
    later.group_total_len = 8;
    later.slice_count = 1;
    inject_advertise(injector, /*node=*/1, later);

    check(wait_until([&] { return tx_b.dropped_groups() >= 1; }, 2s),
          "the stale incomplete group was swept and counted on dropped_groups()");
}

/**
 * @brief #912 — `max_groups` reaches the reassembly buffer at all.
 *
 * The buffer was default-constructed, so `max_groups` was 0 and its evict-oldest
 * could never fire in production however the connection was configured. Three
 * concurrently-incomplete groups against a ceiling of 2 must evict one.
 */
void test_max_groups_reaches_the_reassembly_buffer() {
    std::printf("transport_can max_groups plumbing (injected group ceiling):\n");

    fake_can_bus_t bus;
    fake_link_t injector(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can_config_t cfg = rx_test_config();
    cfg.max_groups = 2;
    cfg.rx_ttl = 60s;  // far out of reach: the COUNT bound must be what evicts
    tr::net::transport_can tx_b(std::move(link_b), cfg);

    // Three distinct 2-slice groups, each left one slice short, so none completes
    // and none is erased by the delivery path.
    for (std::uint16_t g = 0; g < 3; ++g) {
        const std::uint16_t base = static_cast<std::uint16_t>(1 + g * 2);
        can::advertise_t adv;
        adv.can_id = can::encode_can_id({0, 1, base});
        adv.group = true;
        adv.group_total_len = 16;
        adv.slice_count = 2;
        adv.path = "sensor/temp";
        inject_advertise(injector, /*node=*/1, adv);
        inject_data(injector, /*node=*/1, base);  // only slice 0 of 2
    }

    check(wait_until([&] { return tx_b.dropped_groups() >= 1; }, 2s),
          "a 3rd live group against a ceiling of 2 evicted the oldest");
}

/**
 * @brief #910 — a group needing more endpoint slots than exist is refused WHOLE, before
 *        its manifest is emitted, so no receiver is left holding an uncompleteable group.
 *
 * The defect: `emit_advertise` ran BEFORE the per-slice loop that discovers
 * `slice_can_id` has run out of endpoint slots, so the manifest promised `slice_count`
 * slices and the loop `break`ed after delivering fewer. Every listener created a
 * reassembly group keyed on that promise and buffered the partial slices forever.
 *
 * The receiver-side assertion is the load-bearing one, and it is made two ways: no
 * binding was ever learned (nothing reached the bus), and — after a window longer than
 * `rx_ttl`, with a real later advertise driving the stale sweep — `dropped_groups()` is
 * still zero, i.e. there was no pinned group for the sweep to find. A test that only
 * checked `dropped_tx()` would pass against a fix that advertised and then retracted.
 */
void test_oversized_group_is_refused_before_advertising() {
    std::printf("transport_can oversized group refused before the advertise (#910):\n");

    fake_can_bus_t bus;
    sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can_config_t cfg_b = rx_test_config();
    cfg_b.rx_ttl = 30ms;  // short, so any pinned group is provably sweepable below
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "sensor/temp"});
    tr::net::transport_can tx_b(std::move(link_b), cfg_b);
    tx_b.set_receiver(rx);

    // Exactly one slice more than the data-endpoint window holds. Sized from the
    // constants, not from 32768: the bound is the CAN ID's, and if kEndpointBits ever
    // widens this test must follow it rather than silently stop testing the boundary.
    const std::vector<std::byte> huge =
        make_payload((tr::net::kCanMaxGroupSlices + 1) * tr::view::kCanClassicMaxData);
    tx_a.send(huge);

    check(wait_until([&] { return tx_a.dropped_tx() >= 1; }, 2s),
          "the oversized frame was refused whole and counted on dropped_tx()");

    const std::uint32_t base_id = can::encode_can_id({0, 1, tr::net::kCanFirstDataEndpoint});
    std::this_thread::sleep_for(150ms);  // >> rx_ttl, and past anything the bus could carry
    check(!tx_b.learned_binding(base_id).has_value(),
          "no manifest reached the bus — the receiver bound nothing");

    // A real, small group now drives the receiver's stale sweep (every group is born of
    // an advertise, which is the sweep's cadence). Nothing to reclaim => the counter
    // stays at zero; an advertise-then-break leaves a group here and it gets swept.
    const std::vector<std::byte> small = make_payload(24);
    tx_a.send(small);
    check(sink.wait_for_count(1, 2s), "the node is LIVE: the next frame is delivered");
    check(equal_bytes(sink.last(), small), "and its bytes are byte-exact");
    check(tx_b.dropped_groups() == 0,
          "no never-completing group was ever buffered at the receiver");
    check(tx_b.pending_slices() == 0, "and no orphan data slices were parked");
}

/**
 * @brief #910, the silent half — a group over 65535 slices no longer wraps its advertised
 *        count into a different message.
 *
 * `alloc_base` narrowed the slice count to `std::uint16_t` and `adv.slice_count` cast the
 * same `std::size_t`, so 65536 slices became `0` in both places: the reservation trivially
 * "fit", and the manifest went out as the HELLO form (`slice_count == 0`), which binds
 * nothing. The data slices that followed matched no binding at all and every one of them
 * parked in the pending queue. The reservation is now computed in `std::size_t` and any
 * count over `kCanMaxGroupSlices` is refused, which closes the wrap as a special case of
 * the same bound.
 */
void test_u16_slice_count_wrap_cannot_advertise_a_hello() {
    std::printf("transport_can >65535-slice group cannot wrap into a hello (#910):\n");

    fake_can_bus_t bus;
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "sensor/temp"});
    tr::net::transport_can tx_b(std::move(link_b), rx_test_config());

    // One slice past the u16 range — the count that used to narrow to zero.
    const std::size_t slices = std::size_t{0xFFFF} + 1;
    const std::vector<std::byte> huge = make_payload(slices * tr::view::kCanClassicMaxData);
    tx_a.send(huge);

    check(wait_until([&] { return tx_a.dropped_tx() >= 1; }, 2s),
          "the wrapping frame was refused whole and counted on dropped_tx()");
    std::this_thread::sleep_for(150ms);  // let anything that DID reach the bus arrive
    check(tx_b.pending_slices() == 0,
          "no unbindable data slices were emitted, so none parked at the receiver");
    check(tx_b.dropped_groups() == 0, "and no group was created at the receiver");
}

/**
 * @brief #911 — an inbound slice whose bytes cannot be OWNED drops the whole group and
 *        counts it; it is never fabricated into an empty placeholder and delivered short.
 *
 * `tr::view::over_bytes` has two outcomes and `nullopt` means exactly one thing: the
 * backend refused. `.value_or(view_t{})` turned that into an engaged EMPTY view, which
 * the reassembly buffer counts like any other slice — so `is_complete` was satisfied,
 * `assemble` chained the placeholder, and the `min(total, rope->total_length())` trim
 * quietly shortened the result. A byte-wrong, SHORT frame was delivered upstream as
 * valid data.
 *
 * The refusal here comes from a REAL injected backend, not a stub: a `mem::sync_pool_t`
 * over a caller-owned slab, drained to exactly as many free slots as the group needs
 * minus one. That is the production backpressure shape (`alloc` answering `nullptr` on a
 * bounded node), reached through the config seam an embedder actually sets.
 */
void test_rx_slice_refusal_drops_the_group_and_counts() {
    std::printf("transport_can ingress slice refusal drops + counts, never fabricates (#911):\n");

    // The injected byte seam. Slots are one CLASSIC data field wide — the exact size an
    // inbound slice copy asks for.
    alignas(std::max_align_t) std::array<std::byte, 8192> slab{};
    tr::mem::sync_pool_t pool(slab, tr::view::kCanClassicMaxData);

    // Drain to a KNOWN free-slot count so the refusing slice index is exact rather than
    // whatever the slab arithmetic happened to yield. The 3-window group below gets two
    // slots: slices 0 and 1 are owned, slice 2 is refused.
    constexpr std::size_t kKeepFree = 2;
    std::vector<tr::view::segment_ptr_t> held;
    while (tr::view::segment_ptr_t s =
               tr::view::segment_alloc(pool, tr::view::kCanClassicMaxData)) {
        held.push_back(std::move(s));
    }
    check(held.size() > kKeepFree, "the injected pool holds more slots than the group needs");
    held.resize(held.size() - kKeepFree);  // hand exactly kKeepFree slots back

    fake_can_bus_t bus;
    sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can_config_t cfg_b = rx_test_config();
    cfg_b.rx_backend = &pool;
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "sensor/temp"});
    tr::net::transport_can tx_b(std::move(link_b), cfg_b);
    tx_b.set_receiver(rx);

    // 24 bytes => 3 CLASSIC windows against 2 free slots: the third copy is refused.
    const std::vector<std::byte> three_windows = make_payload(24);
    tx_a.send(three_windows);

    check(!sink.wait_for_count(1, 500ms),
          "NOTHING was delivered for the refused group (no short frame)");
    check(wait_until([&] { return tx_b.dropped_rx() >= 1; }, 2s),
          "the refused slice ticked dropped_rx()");
    check(tx_b.dropped_groups() >= 1,
          "and the dead group was reclaimed, counted on dropped_groups()");

    // Reclaiming the group returned its slots, so the node is live again. A DIFFERENT
    // byte pattern on purpose: make_payload is index-derived, so a 16-byte prefix of the
    // refused group's payload would be indistinguishable from a legitimate 16-byte frame
    // and the liveness check could pass on the very corruption it is meant to exclude.
    const std::vector<std::byte> two_windows(2 * tr::view::kCanClassicMaxData, std::byte{0xC3});
    tx_a.send(two_windows);
    check(sink.wait_for_count(1, 2s), "the node is LIVE: the next group is delivered");
    check(equal_bytes(sink.last(), two_windows), "and its bytes are byte-exact");
    check(sink.count() == 1, "exactly ONE frame was ever delivered — nothing short slipped by");
}

/**
 * @brief #911, the second door — a DLC-0 data slice reaches the SAME corruption by the
 *        success path, so it takes the same disposition.
 *
 * The refusal guard above turns on `over_bytes` answering `nullopt`. A zero-length data
 * field never gets there: `over_bytes` returns an ENGAGED empty view for empty input, so
 * a DLC-0 frame walks the success path and inserts exactly the placeholder that guard
 * exists to prevent. The buffer counts entries without inspecting length, `is_complete`
 * fires one slice early, and the `min(total, rope->total_length())` trim delivers a
 * SHORT frame as valid — byte-for-byte the #911 outcome, from a different direction.
 *
 * A conforming sender never emits one, so the vector is a hand-built non-conforming peer:
 * a manifest promising two 8-byte slices, one real slice, and a DLC-0 second.
 */
void test_rx_zero_length_slice_drops_the_group_and_counts() {
    std::printf("transport_can ingress DLC-0 slice drops + counts, never completes (#911):\n");

    fake_can_bus_t bus;
    sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
    fake_link_t injector(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);
    tr::net::transport_can tx_b(std::move(link_b), rx_test_config());
    tx_b.set_receiver(rx);

    can::advertise_t adv;
    adv.can_id = can::encode_can_id({0, 1, tr::net::kCanFirstDataEndpoint});
    adv.group = true;
    adv.group_total_len = 16;  // two full CLASSIC windows
    adv.slice_count = 2;
    adv.path = "sensor/temp";
    inject_advertise(injector, /*node=*/1, adv);

    inject_data(injector, /*node=*/1, /*endpoint=*/tr::net::kCanFirstDataEndpoint);

    // The second slice, declaring no data field at all.
    tr::net::can_frame_data_t empty;
    empty.id = can::encode_can_id({0, 1, tr::net::kCanFirstDataEndpoint + 1});
    empty.fd = false;
    empty.len = 0;
    injector.write_raw(empty);

    check(!sink.wait_for_count(1, 500ms),
          "no SHORT frame was delivered for the group the DLC-0 slice completed");
    check(wait_until([&] { return tx_b.dropped_rx() >= 1; }, 2s),
          "the zero-length slice ticked dropped_rx()");
    check(tx_b.dropped_groups() >= 1,
          "and the group it would have falsely completed was reclaimed on dropped_groups()");

    // Liveness, on a distinct byte pattern so a truncated remnant of the group above
    // could not masquerade as this delivery. The wait is RELATIVE to whatever already
    // arrived: an absolute `wait_for_count(1)` passes instantly in a build that shipped
    // the short frame, so it would report liveness on the very corruption above — the
    // guard has to be able to fail for the reason it names.
    const std::size_t before = sink.count();
    auto link_a = std::make_unique<fake_link_t>(bus);
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 3, tr::view::can_frame_mode_t::CLASSIC, "sensor/other"});
    const std::vector<std::byte> live(2 * tr::view::kCanClassicMaxData, std::byte{0xA7});
    tx_a.send(live);
    check(sink.wait_for_count(before + 1, 2s), "the node is LIVE: a later group is delivered");
    check(equal_bytes(sink.last(), live), "and its bytes are byte-exact");
    check(sink.count() == 1, "exactly ONE frame was ever delivered — nothing short slipped by");
}

/** @brief #912 — a healthy exchange reports zero on every drop counter. */
void test_clean_run_counters_are_zero() {
    std::printf("transport_can drop counters on a clean run:\n");
    fake_can_bus_t bus;
    sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.on(f); };
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "sensor/temp"});
    tr::net::transport_can tx_b(std::move(link_b),
                                {0, 2, tr::view::can_frame_mode_t::CLASSIC, "q"});
    tx_b.set_receiver(rx);
    const std::vector<std::byte> payload = make_payload(40);
    tx_a.send(payload);
    check(sink.wait_for_count(1, 2s), "frame delivered");
    check(tx_b.dropped_rx() == 0 && tx_b.dropped_groups() == 0,
          "receiver reports zero RX drops on a clean run");
    check(tx_a.dropped_tx() == 0, "sender reports zero TX drops on a clean run");
    check(tx_b.pending_slices() == 0, "no slices left parked after delivery");
}

}  // namespace

/**
 * @brief ADR-0053 §5: the owning peer-named sink receives the reassembled group AS THE ROPE IT
 *        ALREADY IS — one link per slice, CAN-FD DLC padding trimmed by SHORTENING the tail link
 *        (never a flatten), bytes byte-exact, zero-copy.
 */
void test_rope_delivery() {
    std::printf("transport_can owning rope delivery (ADR-0053):\n");

    fake_can_bus_t bus;
    // Capture state + named peer-rope lambda BEFORE the transports (the slot
    // binds the callable by address; ~transport_can joins its receive thread).
    std::mutex m;
    std::condition_variable cv;
    std::string peer;
    std::optional<tr::view::rope_t> got;
    auto peer_rope_rx = [&](std::string_view name, tr::view::rope_t frame) {
        const std::lock_guard lock(m);
        peer = name;
        got = std::move(frame);  // the refcounted links outlive the callback
        cv.notify_all();
    };
    auto link_a = std::make_unique<fake_link_t>(bus);
    auto link_b = std::make_unique<fake_link_t>(bus);

    tr::net::transport_can tx_a(std::move(link_a), {0, 1, tr::view::can_frame_mode_t::FD, "p"});
    tr::net::transport_can tx_b(std::move(link_b), {0, 2, tr::view::can_frame_mode_t::FD, "q"});

    check(tx_b.delivers_ropes(), "transport_can::delivers_ropes() is true");

    tx_b.set_peer_rope_receiver(peer_rope_rx);

    // 100 bytes in FD: slices 64 + 36; the 36-byte tail was padded to DLC 48 on
    // the wire, so the trim must SHORTEN the second link back to 36.
    const std::vector<std::byte> payload = make_payload(100);
    tx_a.send(payload);

    std::unique_lock lock(m);
    const bool arrived = cv.wait_for(lock, 2s, [&] { return got.has_value(); });
    check(arrived, "group delivered to the owning peer-named sink");
    if (!arrived) return;

    check(peer == "n1", "delivered under the SENDER's peer name");
    check(got->total_length() == payload.size(), "rope length == advertised total (trimmed)");
    check(got->link_count() == 2, "one link per slice (no flatten, no re-chaining)");
    if (got->link_count() == 2) {
        check(got->links()[0].length == 64 && got->links()[1].length == 36,
              "DLC padding removed by shortening the tail link");
    }
    std::vector<std::byte> flatbytes;
    got->walk([&](std::span<const std::byte> sp) {
        flatbytes.insert(flatbytes.end(), sp.begin(), sp.end());
    });
    check(equal_bytes(flatbytes, payload), "rope bytes are byte-exact");
}

int main() {
    test_roundtrip(tr::view::can_frame_mode_t::CLASSIC, 20, "classic, multi-frame");
    test_roundtrip(tr::view::can_frame_mode_t::FD, 150, "CAN-FD, multi-frame");
    test_single_value();
    test_fd_dlc_padding();
    test_rope_delivery();
    test_control_stream_resync();
    test_lifecycle();
    test_clean_run_counters_are_zero();
    test_pending_flood_is_capped_and_counted();
    test_pending_slices_age_out();
    test_zero_peer_ttl_does_not_disable_the_age_out();
    test_incomplete_group_is_swept();
    test_max_groups_reaches_the_reassembly_buffer();
    test_oversized_group_is_refused_before_advertising();
    test_u16_slice_count_wrap_cannot_advertise_a_hello();
    test_rx_slice_refusal_drops_the_group_and_counts();
    test_rx_zero_length_slice_drops_the_group_and_counts();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
