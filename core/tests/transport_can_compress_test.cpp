/**
 * @file
 * @brief #1953 — CAN link-local compression: announced per link, private to the transport.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two CAN ends that both announce the capability move a directed stream onto native CAN
 * identifiers: its second send binds its prefix (the FWD outer header and `op`/`dst`/`src`
 * children) to a block of the sender's stream-window identifiers, the receiver holds it and
 * acknowledges, and every later send is the rest of the frame on those identifiers, with no
 * manifest. The receiver puts the prefix back, so the frame handed upward is byte-identical
 * to the one the sender was given. A peer that does not announce the capability gets full
 * frames and the legacy wire, byte for byte.
 *
 * The bus here is PUMPED, not threaded: `write_raw` only queues, and the test drains the
 * queue on its own thread after each send. That keeps every byte count exact and every
 * interleaving deterministic, while the transport still sees what it sees on a real bus —
 * an inbound frame callback that may itself transmit (an acknowledgement, a hello).
 *
 * Coverage:
 *   - compressed frames round-trip byte-exact, classic and CAN-FD, across payload sizes;
 *   - a native send of a 4-byte value is one classic frame;
 *   - a peer without the capability gets the legacy wire exactly;
 *   - a bystander drops native frames without delivering, parking or counting anything;
 *   - broadcast sends, non-FWD frames and one-shot frames are never compressed;
 *   - a restarted receiver resets the link (its hello), and a lost hello is recovered;
 *   - a refusal is directed: it unbinds the refused stream and no third node's;
 *   - the stream window is bounded: a stream it cannot hold travels whole, none is evicted;
 *   - a native send that loses a slice is dropped, never welded to the next;
 *   - the advertise codec carries the link flags and an older decoder ignores them.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/can.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/transport_can.hpp"
#include "test_support.hpp"

namespace {

namespace can = tr::net::can;
using tr::testing::check;
using bytes_t = std::vector<std::byte>;

class pump_link_t;

/**
 * @brief A deterministic in-memory bus: `write_raw` queues, @ref pump delivers.
 *
 * Counts every frame and every data-field byte by the node that emitted it, so a test
 * reads exactly what one send cost on the wire.
 */
class pump_bus_t {
   public:
    /** @brief Wire cost attributed to one emitting node. */
    struct cost_t {
        std::size_t frames = 0; /**< @brief CAN frames emitted. */
        std::size_t bytes = 0;  /**< @brief Data-field bytes emitted (post DLC pad). */
    };

    void attach(pump_link_t* l) { links_.push_back(l); }
    void detach(pump_link_t* l) {
        std::erase(links_, l);
        for (auto& q : q_)
            if (q.first == l) q.first = nullptr;
    }
    void post(pump_link_t* from, const tr::net::can_frame_data_t& f) {
        const auto fields = can::decode_can_id(f.id);
        if (drop_ && drop_(f)) return;
        if (fields) {
            cost_t& c = cost_[fields->node];
            ++c.frames;
            c.bytes += f.len;
        }
        q_.emplace_back(from, f);
    }
    /** @brief Deliver every queued frame, including those emitted while delivering. */
    void pump();
    /** @brief The bus loses every frame @p pred accepts (`nullptr` = lossless). */
    void set_drop(std::function<bool(const tr::net::can_frame_data_t&)> pred) {
        drop_ = std::move(pred);
    }
    [[nodiscard]] cost_t cost(std::uint16_t node) const {
        const auto it = cost_.find(node);
        return it == cost_.end() ? cost_t{} : it->second;
    }

   private:
    std::vector<pump_link_t*> links_;
    std::deque<std::pair<pump_link_t*, tr::net::can_frame_data_t>> q_;
    std::function<bool(const tr::net::can_frame_data_t&)> drop_;
    std::map<std::uint16_t, cost_t> cost_;
};

/** @brief The @ref tr::net::can_link_t over a @ref pump_bus_t. */
class pump_link_t : public tr::net::can_link_t {
   public:
    explicit pump_link_t(pump_bus_t& bus) : bus_(bus) { bus_.attach(this); }
    ~pump_link_t() override { bus_.detach(this); }
    pump_link_t(const pump_link_t&) = delete;
    pump_link_t& operator=(const pump_link_t&) = delete;

    void write_raw(const tr::net::can_frame_data_t& f) override { bus_.post(this, f); }
    void on_receive(rx_fn_t rx) override { rx_ = std::move(rx); }
    void start() override { started_ = true; }
    void deliver(const tr::net::can_frame_data_t& f) {
        if (started_ && rx_) rx_(f);
    }

   private:
    pump_bus_t& bus_;
    rx_fn_t rx_;
    bool started_ = false;
};

void pump_bus_t::pump() {
    while (!q_.empty()) {
        const auto [from, f] = q_.front();
        q_.pop_front();
        const std::vector<pump_link_t*> snapshot = links_;
        for (pump_link_t* l : snapshot)
            if (l != from) l->deliver(f);
    }
}

/** @brief Captures every frame a transport delivers upward, in order. */
struct sink_t {
    std::vector<bytes_t> frames;
    void operator()(std::span<const std::byte> f) { frames.emplace_back(f.begin(), f.end()); }
};

/** @brief A configured transport on the pumped bus. */
std::unique_ptr<tr::net::can_transport_t> make_node(
    pump_bus_t& bus, std::uint16_t node, std::string_view path,
    std::size_t compress_ids = tr::net::kCanStreamIds,
    can::can_frame_mode_t mode = can::can_frame_mode_t::CLASSIC) {
    tr::net::transport_can_config_t cfg;
    cfg.node = node;
    cfg.mode = mode;
    cfg.path = path;
    cfg.compress_ids = compress_ids;
    return std::make_unique<tr::net::can_transport_t>(
        tr::mem::make_poly<pump_link_t>(tr::mem::net_source(), bus), cfg);
}

/** @brief A VALUE TLV of @p n deterministic bytes, salted by @p salt. */
bytes_t value_tlv(std::size_t n, std::uint8_t salt) {
    bytes_t body(n);
    for (std::size_t i = 0; i < n; ++i) body[i] = static_cast<std::byte>((i * 13 + salt) & 0xFF);
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{.ll = n > 0xFFFF}, body);
    return out;
}

/** @brief A delivery-shaped FWD{WRITE} to @p dst with an empty `src` and a VALUE payload. */
bytes_t delivery(std::initializer_list<std::string_view> dst, std::size_t payload,
                 std::uint8_t salt) {
    const bytes_t d = tr::testing::b_path(dst);
    const bytes_t s = tr::testing::b_path(std::initializer_list<std::string_view>{});
    const bytes_t v = value_tlv(payload, salt);
    return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, d, s, {}, v);
}

/** @brief Wire cost of a legacy (uncompressed) group: manifest + path, then the frame. */
pump_bus_t::cost_t legacy_cost(std::size_t frame_len, std::size_t path_len) {
    const std::size_t manifest = can::kAdvertiseHeaderSize + path_len;
    const std::size_t c = can::kCanClassicMaxData;
    return {(manifest + c - 1) / c + (frame_len + c - 1) / c, manifest + frame_len};
}

/** @brief The wire cost node @p node paid for whatever ran inside @p fn. */
template <class Fn>
pump_bus_t::cost_t cost_of(pump_bus_t& bus, std::uint16_t node, Fn&& fn) {
    const pump_bus_t::cost_t before = bus.cost(node);
    fn();
    bus.pump();
    const pump_bus_t::cost_t after = bus.cost(node);
    return {after.frames - before.frames, after.bytes - before.bytes};
}

// ---------------------------------------------------------------------------

void test_compressed_frames_round_trip(can::can_frame_mode_t mode, const char* label) {
    std::printf("compressed stream round-trips byte-exact (%s)\n", label);
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor", tr::net::kCanStreamIds, mode);
    auto b = make_node(bus, 2, "logger", tr::net::kCanStreamIds, mode);
    auto c = make_node(bus, 3, "bystander", tr::net::kCanStreamIds, mode);
    sink_t at_b, at_c;
    b->set_receiver(at_b);
    c->set_receiver(at_c);
    bus.pump();  // the join hellos: every node learns every other
    tr::net::transport_t* const to_b = a->peer_link("n2");
    check(to_b != nullptr, "A resolves its peer n2 after the hellos");
    if (to_b == nullptr) return;

    std::vector<bytes_t> sent;
    std::vector<pump_bus_t::cost_t> costs;
    const std::size_t sizes[] = {4, 4, 4, 64, 1024, 4096, 16384, 4};
    std::uint8_t salt = 0;
    for (const std::size_t n : sizes) {
        sent.push_back(delivery({"telemetry", "imu", "accel"}, n, ++salt));
        costs.push_back(cost_of(bus, 1, [&] { to_b->send(sent.back()); }));
    }
    check(at_b.frames == sent, "B received every frame, byte-identical and in order");
    check(at_c.frames.empty(), "the bystander delivered nothing");
    check(c->dropped_rx() == 0 && c->pending_slices() == 0 && c->dropped_groups() == 0,
          "the bystander consumed every group without parking or counting a slice");
    check(b->dropped_rx() == 0 && b->dropped_groups() == 0, "B dropped nothing");
    // Send 0 is the stream's first sighting (whole), send 1 binds it, send 2 is native: the
    // 8-byte VALUE TLV alone, on the stream's own identifier.
    check(costs[2].frames == 1 && costs[2].bytes == 8 && costs[7].frames == 1,
          "a native send of a 4-byte value is one frame of eight bytes");
}

void test_peer_without_compression_gets_full_frames() {
    std::printf("a peer without compression gets the legacy wire, byte for byte\n");
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor");
    auto b = make_node(bus, 2, "logger", 0);  // compression off: never announces it
    sink_t at_b;
    b->set_receiver(at_b);
    bus.pump();
    tr::net::transport_t* const to_b = a->peer_link("n2");
    check(to_b != nullptr, "A resolves n2");
    if (to_b == nullptr) return;
    std::vector<bytes_t> sent;
    bool legacy = true;
    for (std::uint8_t i = 0; i < 5; ++i) {
        sent.push_back(delivery({"telemetry", "imu", "accel"}, 8 + i, i));
        const pump_bus_t::cost_t got = cost_of(bus, 1, [&] { to_b->send(sent.back()); });
        const pump_bus_t::cost_t want = legacy_cost(sent.back().size(), std::strlen("sensor"));
        legacy = legacy && got.bytes == want.bytes && got.frames == want.frames;
    }
    check(at_b.frames == sent, "B received every frame byte-identical");
    check(legacy, "every send cost exactly the legacy manifest-plus-frame");
    check(bus.cost(2).frames == legacy_cost(0, std::strlen("logger")).frames,
          "B sent nothing but its join hello (no acknowledgement)");
}

void test_broadcast_and_non_fwd_are_never_compressed() {
    std::printf("broadcast sends and non-FWD frames keep the legacy wire\n");
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor");
    auto b = make_node(bus, 2, "logger");
    sink_t at_b;
    b->set_receiver(at_b);
    bus.pump();
    tr::net::transport_t* const to_b = a->peer_link("n2");
    if (to_b == nullptr) {
        check(false, "A resolves n2");
        return;
    }
    const bytes_t fwd = delivery({"telemetry", "imu"}, 16, 7);
    bool legacy = true;
    for (int i = 0; i < 3; ++i) {
        const pump_bus_t::cost_t got = cost_of(bus, 1, [&] { a->send(fwd); });
        const pump_bus_t::cost_t want = legacy_cost(fwd.size(), std::strlen("sensor"));
        legacy = legacy && got.bytes == want.bytes && got.frames == want.frames;
    }
    check(legacy, "a broadcast send is a full frame every time");
    const bytes_t opaque = value_tlv(40, 3);  // a bare VALUE: not a FWD, nothing to elide
    for (int i = 0; i < 3; ++i) {
        const pump_bus_t::cost_t got = cost_of(bus, 1, [&] { to_b->send(opaque); });
        const pump_bus_t::cost_t want = legacy_cost(opaque.size(), std::strlen("sensor"));
        legacy = legacy && got.bytes == want.bytes && got.frames == want.frames;
    }
    check(legacy, "a directed non-FWD frame is a full frame every time");
    check(at_b.frames.size() == 6 && at_b.frames.back() == opaque, "all six were delivered");
}

void test_restarted_receiver_resets_the_link() {
    std::printf("a restarted receiver's hello resets the link\n");
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor");
    auto b = make_node(bus, 2, "logger");
    sink_t at_b;
    b->set_receiver(at_b);
    bus.pump();
    tr::net::transport_t* const to_b = a->peer_link("n2");
    if (to_b == nullptr) {
        check(false, "A resolves n2");
        return;
    }
    for (std::uint8_t i = 0; i < 3; ++i) {
        to_b->send(delivery({"t", "x"}, 8, i));
        bus.pump();
    }
    check(at_b.frames.size() == 3, "the stream is established and compressed");
    b.reset();  // the link to n2 dies with B's transport
    auto b2 = make_node(bus, 2, "logger");
    sink_t at_b2;
    b2->set_receiver(at_b2);
    bus.pump();  // B2's join hello reaches A
    std::vector<bytes_t> sent;
    for (std::uint8_t i = 0; i < 4; ++i) {
        sent.push_back(delivery({"t", "x"}, 8, static_cast<std::uint8_t>(40 + i)));
        to_b->send(sent.back());
        bus.pump();
    }
    check(at_b2.frames == sent, "the fresh receiver got every frame, byte-identical");
    check(b2->dropped_rx() == 0, "and refused none: the hello reset A before it compressed");
}

void test_lost_hello_is_recovered() {
    std::printf("a lost hello costs one refused frame, then every stream recovers at once\n");
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor");
    auto b = make_node(bus, 2, "logger");
    sink_t at_b;
    b->set_receiver(at_b);
    bus.pump();
    tr::net::transport_t* const to_b = a->peer_link("n2");
    if (to_b == nullptr) {
        check(false, "A resolves n2");
        return;
    }
    const char* const leaves[] = {"a", "b", "c", "d", "e"};
    for (std::uint8_t round = 0; round < 3; ++round) {
        for (const char* leaf : leaves) {
            to_b->send(delivery({"t", leaf}, 8, round));
            bus.pump();
        }
    }
    b.reset();
    // The restarted receiver's join hello is lost on the bus.
    bus.set_drop([](const tr::net::can_frame_data_t& f) {
        const auto fields = can::decode_can_id(f.id);
        return fields && fields->node == 2;
    });
    auto b2 = make_node(bus, 2, "logger");
    sink_t at_b2;
    b2->set_receiver(at_b2);
    bus.pump();
    bus.set_drop(nullptr);
    std::vector<bytes_t> sent;
    for (std::uint8_t round = 0; round < 3; ++round) {
        for (const char* leaf : leaves) {
            sent.push_back(delivery({"t", leaf}, 8, static_cast<std::uint8_t>(60 + round)));
            to_b->send(sent.back());
            bus.pump();
        }
    }
    // The one refusal says the receiver lost what it held, so the whole link resets: the
    // other four streams bind again on their next send instead of each losing a frame.
    check(at_b2.frames.size() == sent.size() - 1 &&
              std::equal(at_b2.frames.begin(), at_b2.frames.end(), sent.begin() + 1),
          "only the first native frame was lost; every later frame arrived byte-identical");
}

void test_stream_window_is_bounded() {
    std::printf("a stream the window cannot hold travels whole; none is evicted\n");
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor", 2);  // two streams per peer
    auto b = make_node(bus, 2, "logger", 2);
    sink_t at_b;
    b->set_receiver(at_b);
    bus.pump();
    tr::net::transport_t* const to_b = a->peer_link("n2");
    if (to_b == nullptr) {
        check(false, "A resolves n2");
        return;
    }
    std::vector<bytes_t> sent;
    const char* const leaves[] = {"a", "b", "c"};
    for (std::uint8_t i = 0; i < 24; ++i) {
        sent.push_back(delivery({"s", leaves[i % 3]}, 8 + (i % 5), i));
        to_b->send(sent.back());
        bus.pump();
    }
    check(at_b.frames == sent, "many streams over a two-identifier window: every frame byte-exact");
    check(b->dropped_rx() == 0, "and nothing was refused");

    // Two one-frame streams fill a two-identifier window; a third travels whole, and the
    // first two stay native.
    pump_bus_t bus2;
    auto a2 = make_node(bus2, 1, "sensor", 2);
    auto b2 = make_node(bus2, 2, "logger", 2);
    sink_t at_b2;
    b2->set_receiver(at_b2);
    bus2.pump();
    tr::net::transport_t* const to_b2 = a2->peer_link("n2");
    if (to_b2 == nullptr) return;
    std::vector<bytes_t> sent2;
    std::size_t frames[3] = {};
    for (std::uint8_t round = 0; round < 4; ++round) {
        for (std::size_t i = 0; i < 3; ++i) {
            sent2.push_back(delivery({"s", leaves[i]}, 4, round));
            frames[i] = cost_of(bus2, 1, [&] { to_b2->send(sent2.back()); }).frames;
        }
    }
    check(at_b2.frames == sent2, "every frame byte-exact");
    check(frames[0] == 1 && frames[1] == 1, "the two streams that hold the window stay native");
    check(frames[2] == legacy_cost(sent2.back().size(), std::strlen("sensor")).frames,
          "the third, with no identifier free, is a full frame");
}

void test_one_shot_frames_cost_nothing_extra() {
    std::printf("a frame seen once is sent whole: no bind, no acknowledgement\n");
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor");
    auto b = make_node(bus, 2, "logger");
    sink_t at_b;
    b->set_receiver(at_b);
    bus.pump();
    tr::net::transport_t* const to_b = a->peer_link("n2");
    if (to_b == nullptr) {
        check(false, "A resolves n2");
        return;
    }
    const pump_bus_t::cost_t b_before = bus.cost(2);
    bool legacy = true;
    for (int i = 0; i < 20; ++i) {
        const std::string leaf = "leaf" + std::to_string(i);
        const bytes_t f = delivery({"walk", leaf}, 4, static_cast<std::uint8_t>(i));
        const pump_bus_t::cost_t got = cost_of(bus, 1, [&] { to_b->send(f); });
        const pump_bus_t::cost_t want = legacy_cost(f.size(), std::strlen("sensor"));
        legacy = legacy && got.bytes == want.bytes && got.frames == want.frames;
    }
    check(legacy, "twenty one-shot frames cost exactly the legacy wire");
    check(bus.cost(2).frames == b_before.frames, "and the receiver sent nothing back");
    check(at_b.frames.size() == 20, "all delivered");
}

void test_refusal_is_directed() {
    std::printf("a refusal unbinds the refused stream and leaves a third node's intact\n");
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor");
    auto b = make_node(bus, 2, "logger");
    auto c = make_node(bus, 3, "display");
    sink_t at_b, at_c;
    b->set_receiver(at_b);
    c->set_receiver(at_c);
    bus.pump();
    tr::net::transport_t* const to_b = a->peer_link("n2");
    tr::net::transport_t* const to_c = a->peer_link("n3");
    if (to_b == nullptr || to_c == nullptr) {
        check(false, "A resolves n2 and n3");
        return;
    }
    std::vector<bytes_t> sent_c;
    for (std::uint8_t i = 0; i < 3; ++i) {
        to_b->send(delivery({"t", "x"}, 4, i));
        sent_c.push_back(delivery({"t", "y"}, 4, i));
        to_c->send(sent_c.back());
        bus.pump();
    }
    check(cost_of(bus, 1, [&] { to_c->send(sent_c.emplace_back(delivery({"t", "y"}, 4, 9))); })
                  .frames == 1,
          "A to C is native");
    // B restarts and A misses its join hello: B no longer holds A's stream.
    b.reset();
    bus.set_drop([](const tr::net::can_frame_data_t& f) {
        const auto fields = can::decode_can_id(f.id);
        return fields && fields->node == 2;
    });
    auto b2 = make_node(bus, 2, "logger");
    sink_t at_b2;
    b2->set_receiver(at_b2);
    bus.pump();
    bus.set_drop(nullptr);
    std::vector<bytes_t> sent_b;
    for (std::uint8_t i = 0; i < 4; ++i) {
        sent_b.push_back(delivery({"t", "x"}, 4, static_cast<std::uint8_t>(20 + i)));
        to_b->send(sent_b.back());
        bus.pump();
    }
    check(at_b2.frames.size() == 3 &&
              std::equal(at_b2.frames.begin(), at_b2.frames.end(), sent_b.begin() + 1),
          "B's refusal unbound A's stream to B, which recovered after one frame");
    const pump_bus_t::cost_t to_c_cost =
        cost_of(bus, 1, [&] { to_c->send(sent_c.emplace_back(delivery({"t", "y"}, 4, 10))); });
    check(to_c_cost.frames == 1, "A to C is still native: no refusal touched C's stream");
    check(at_c.frames == sent_c, "C received every frame of its own, and none of B's");
    check(c->dropped_rx() == 0 && c->pending_slices() == 0, "C parked and counted nothing");
}

void test_lost_slice_is_never_welded() {
    std::printf("a native send that loses a slice is dropped, never welded to the next\n");
    pump_bus_t bus;
    auto a = make_node(bus, 1, "sensor");
    auto b = make_node(bus, 2, "logger");
    sink_t at_b;
    b->set_receiver(at_b);
    bus.pump();
    tr::net::transport_t* const to_b = a->peer_link("n2");
    if (to_b == nullptr) {
        check(false, "A resolves n2");
        return;
    }
    // A 12-byte value: the native remainder (16 bytes) is two classic slices.
    std::vector<bytes_t> sent;
    for (std::uint8_t i = 0; i < 3; ++i) {
        sent.push_back(delivery({"t", "z"}, 12, i));
        to_b->send(sent.back());
        bus.pump();
    }
    check(at_b.frames == sent, "the stream is native");
    // Lose the second slice of one send.
    int seen = 0;
    bus.set_drop([&](const tr::net::can_frame_data_t& f) {
        const auto fields = can::decode_can_id(f.id);
        return fields && fields->node == 1 && fields->endpoint < tr::net::kCanFirstGroupEndpoint &&
               ++seen == 2;
    });
    to_b->send(delivery({"t", "z"}, 12, 50));
    bus.pump();
    bus.set_drop(nullptr);
    for (std::uint8_t i = 0; i < 3; ++i) {
        sent.push_back(delivery({"t", "z"}, 12, static_cast<std::uint8_t>(60 + i)));
        to_b->send(sent.back());
        bus.pump();
    }
    // The next send's first slice finds the damaged one's partial group and refuses it, which
    // discards both: a native slice carries no sequence number, so only an in-order run of
    // one send's own slices is ever held. Every frame delivered is byte-exact.
    const std::vector<bytes_t> want = {sent[0], sent[1], sent[2], sent[4], sent[5]};
    check(at_b.frames == want,
          "the damaged send and the one after it were dropped; nothing was corrupted");
    check(b->dropped_rx() >= 1, "and the drops were counted");
}

void test_advertise_codec_carries_link_flags() {
    std::printf("the advertise codec carries the link flags; the group bit is unchanged\n");
    can::advertise_t adv;
    adv.can_id = can::encode_can_id({0, 5, 1});
    adv.group = true;
    adv.group_total_len = 99;
    adv.slice_count = 13;
    adv.target = 7;
    adv.link_flags = can::kAdvertiseFlagLinkCompress | can::kAdvertiseFlagStreamRefused;
    tr::mem::bytes_t wire(tr::mem::heap_source());
    check(can::encode_advertise(wire, adv), "it encodes");
    const auto back = can::decode_advertise(std::span<const std::byte>(wire.data(), wire.size()));
    check(back && back->first == adv, "it decodes to the same advertise, link flags included");
    check(std::to_integer<std::uint8_t>(wire.data()[2]) ==
              (can::kAdvertiseFlagGroup | adv.link_flags),
          "the flags byte is the group bit OR the link flags");
    adv.link_flags = 0;
    check(can::encode_advertise(wire, adv) &&
              std::to_integer<std::uint8_t>(wire.data()[2]) == can::kAdvertiseFlagGroup,
          "with no link flags the byte is what it always was");
}

}  // namespace

int main() {
    test_compressed_frames_round_trip(can::can_frame_mode_t::CLASSIC, "classic");
    test_compressed_frames_round_trip(can::can_frame_mode_t::FD, "CAN-FD");
    test_peer_without_compression_gets_full_frames();
    test_broadcast_and_non_fwd_are_never_compressed();
    test_restarted_receiver_resets_the_link();
    test_lost_hello_is_recovered();
    test_stream_window_is_bounded();
    test_one_shot_frames_cost_nothing_extra();
    test_refusal_is_directed();
    test_lost_slice_is_never_welded();
    test_advertise_codec_carries_link_flags();
    return tr::testing::summary("transport_can_compress");
}
