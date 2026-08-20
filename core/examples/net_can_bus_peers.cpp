/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `can` is the one BUS kind: many peers share one wire, so the link
 *        exposes them itself through @ref tr::net::bus_link_t, synthesized from live
 *        traffic — no peer ever becomes a vertex, a table row the graph owns, or any other
 *        stored state (ADR-0044 §1).
 *
 * Every other kind in the tree is point-to-point: one link, one far end, so the child NAME
 * the router registered for the link already addresses it. A CAN bus breaks that — one link
 * reaches every node on the wire — and the routing plane needs a hop segment per peer
 * anyway. The bus facet is how a kind answers that without the graph growing per peer:
 *
 *  - `enumerate_peers` walks a LAST-HEARD table refreshed by other nodes' own traffic and
 *    seeded by the hello advertise a node emits at join. It is a snapshot, not a registry:
 *    a node silent longer than `peer_ttl` simply stops being listed, and nothing had to
 *    notice it leave. There is no join protocol and no coordinator.
 *  - Names are `n<node-id>` — DERIVED from the structured CAN ID rather than assigned, so
 *    they are collision-safe by construction and a rejoining node reappears under the name
 *    it had. (Contrast the stream servers, which name peers `p<slot>` POSITIONALLY: those
 *    names are about the slot, not the session, which is why a pointer to one must be
 *    re-resolved per use.)
 *  - `peer_link(name)` hands back a directed sending endpoint. The medium is still a
 *    broadcast one — every node sees the CAN frames — but the group's advertise carries
 *    `target_node`, so only the addressed peer reassembles and delivers it.
 *
 * The bus here is in-memory. That is not a shortcut but the point of the `can_link_t` seam:
 * raw frame I/O is one virtual, so the transport — framing, reassembly, peer table, the lot
 * — is exercised with no kernel CAN, and the ESP-IDF port swaps `twai_link_t` in at the same
 * seam that `socketcan_link_t` occupies on Linux.
 *
 * Needs the CAN transport (`LIBTRACER_TRANSPORT_CAN`, on by default). It implies the bus
 * module: `transport_can.cpp` carries a `static_assert(kBusLinks)`, because a CAN link is
 * peer-named by construction, so this example can never be built into a target where its
 * subject is absent. Runs under ctest as `example_net_can_bus_peers`; returns non-zero on
 * any failed check.
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/peer_handle.hpp"
#include "libtracer/transport_can.hpp"
#include "libtracer/view_can.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::can_frame_data_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

class fake_link_t;

/** @brief An in-memory CAN wire: whatever one link writes, every OTHER link hears. */
class fake_bus_t {
   public:
    /** @brief Join @p l to the wire. */
    void attach(fake_link_t* l);
    /** @brief Remove @p l from the wire. */
    void detach(fake_link_t* l);
    /** @brief Deliver @p f to every attached link except @p from — the broadcast medium. */
    void broadcast(fake_link_t* from, const can_frame_data_t& f);

   private:
    mutable std::mutex m_;
    std::vector<fake_link_t*> links_;
};

/**
 * @brief One node's raw-frame link — the whole `can_link_t` seam, in memory.
 *
 * Two-phase by contract (#1186): construction only opens the link, and nothing is delivered
 * until @ref start, which `transport_can` calls for its owner after installing the receiver.
 */
class fake_link_t final : public tr::net::can_link_t {
   public:
    /** @brief Open a link onto @p bus. */
    explicit fake_link_t(fake_bus_t& bus) : bus_(bus) { bus_.attach(this); }
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

    void write_raw(const can_frame_data_t& f) override { bus_.broadcast(this, f); }
    void on_receive(rx_fn_t rx) override {
        const std::lock_guard lock(m_);
        rx_ = std::move(rx);
    }
    void start() override {
        if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
    }

    /** @brief Queue @p f for this link's receive thread (called by the bus). */
    void enqueue(const can_frame_data_t& f) {
        {
            const std::lock_guard lock(m_);
            q_.push_back(f);
        }
        cv_.notify_one();
    }

   private:
    /** @brief The receive thread: drain the queue into the registered sink. */
    void run() {
        std::unique_lock lock(m_);
        while (true) {
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) return;
            const can_frame_data_t f = q_.front();
            q_.pop_front();
            const rx_fn_t rx = rx_;
            lock.unlock();
            if (rx) rx(f);
            lock.lock();
        }
    }

    fake_bus_t& bus_;
    rx_fn_t rx_;
    std::deque<can_frame_data_t> q_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread worker_;
};

void fake_bus_t::attach(fake_link_t* l) {
    const std::lock_guard lock(m_);
    links_.push_back(l);
}

void fake_bus_t::detach(fake_link_t* l) {
    const std::lock_guard lock(m_);
    for (auto it = links_.begin(); it != links_.end(); ++it) {
        if (*it == l) {
            links_.erase(it);
            break;
        }
    }
}

void fake_bus_t::broadcast(fake_link_t* from, const can_frame_data_t& f) {
    const std::lock_guard lock(m_);
    for (auto* l : links_)
        if (l != from) l->enqueue(f);
}

/** @brief A thread-safe frame counter for one node's inbound frames. */
class sink_t {
   public:
    /** @brief The receiver callback — copies the span, which dies when it returns. */
    void operator()(std::span<const std::byte> frame) {
        {
            const std::lock_guard lock(m_);
            frames_.emplace_back(frame.begin(), frame.end());
        }
        cv_.notify_all();
    }

    /** @brief Wait until at least @p n frames have landed, or @p budget expires. */
    [[nodiscard]] bool wait_for(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [&] { return frames_.size() >= n; });
    }

    /** @brief How many frames have landed so far. */
    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(m_);
        return frames_.size();
    }

    /** @brief Frame @p i, by value. */
    [[nodiscard]] std::vector<std::byte> at(std::size_t i) const {
        const std::lock_guard lock(m_);
        return frames_.at(i);
    }

   private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::vector<std::byte>> frames_;
};

/**
 * @brief A PEER-NAMED sink: every delivery arrives tagged with the sending peer's handle.
 *
 * This is the bus's own inbound seam (`bus_link_t::set_peer_receiver`) rather than the flat
 * `transport_t` one, and the difference is the whole reason it exists. The flat sink is
 * handed bytes and nothing else — on a point-to-point link that is complete, because the
 * link's registered child NAME already says who the far side is. On a bus it is not, and the
 * handle is what closes the gap. It is a HANDLE and not a name because a name is a string
 * the consumer would have to re-derive an identity from on every frame; @ref peer_name is
 * the one bridge, called here inside the delivery where the answer is defined.
 */
class named_sink_t {
   public:
    /** @brief Bind the link whose deliveries this sink will name; call before frames flow. */
    void bind(tr::net::transport_can& link) { link_ = &link; }

    /** @brief The peer-named receiver callback — record who sent @p frame, then @p frame. */
    void operator()(tr::net::peer_handle_t peer, std::span<const std::byte> frame) {
        char scratch[tr::net::kPeerNameChars];
        std::string sender;
        if (link_ != nullptr) sender = link_->peer_name(peer, scratch);
        {
            const std::lock_guard lock(m_);
            senders_.push_back(std::move(sender));
            frames_.emplace_back(frame.begin(), frame.end());
        }
        cv_.notify_all();
    }

    /** @brief Wait until at least @p n frames have landed, or @p budget expires. */
    [[nodiscard]] bool wait_for(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [&] { return frames_.size() >= n; });
    }

    /** @brief How many frames have landed so far. */
    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(m_);
        return frames_.size();
    }

    /** @brief Frame @p i, by value. */
    [[nodiscard]] std::vector<std::byte> at(std::size_t i) const {
        const std::lock_guard lock(m_);
        return frames_.at(i);
    }

    /** @brief The peer name frame @p i arrived from. */
    [[nodiscard]] std::string sender(std::size_t i) const {
        const std::lock_guard lock(m_);
        return senders_.at(i);
    }

   private:
    tr::net::transport_can* link_ = nullptr;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::vector<std::byte>> frames_;
    std::vector<std::string> senders_;
};

/** @brief A `transport_can` node on @p bus with id @p node, advertising @p path. */
std::unique_ptr<tr::net::transport_can> make_node(fake_bus_t& bus, std::uint16_t node,
                                                  std::string path) {
    tr::net::transport_can_config_t cfg;
    cfg.node = node;
    cfg.mode = tr::view::can_frame_mode_t::CLASSIC;
    cfg.path = std::move(path);
    return std::make_unique<tr::net::transport_can>(std::make_unique<fake_link_t>(bus), cfg);
}

/** @brief The peer names @p link currently hears, in enumeration order. */
std::vector<std::string> audible(tr::net::bus_link_t& link) {
    std::vector<std::string> names;
    link.enumerate_peers([&](std::string_view n) { names.emplace_back(n); });
    return names;
}

/**
 * @brief Poll until @p link hears at least @p n peers, or @p budget expires.
 *
 * A poll and not a wait, deliberately: peer presence is a LIVENESS observation derived from
 * traffic, so there is no edge to wait on — a peer becomes audible because a frame happened
 * to arrive, and stops being audible because nothing did. The bounded loop is the honest
 * shape for that; it is not a stand-in for a rendezvous the API offers and this skipped.
 */
bool wait_for_peers(tr::net::bus_link_t& link, std::size_t n, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (audible(link).size() >= n) return true;
        std::this_thread::sleep_for(2ms);
    }
    return audible(link).size() >= n;
}

/** @brief @p n bytes counting up from @p seed — a stand-in for an encoded frame. */
std::vector<std::byte> frame_of(std::size_t n, unsigned seed) {
    std::vector<std::byte> f(n);
    for (std::size_t i = 0; i < n; ++i) f[i] = static_cast<std::byte>(seed + i);
    return f;
}

}  // namespace

int main() {
    bool ok = true;
    fake_bus_t bus;

    // Three nodes on ONE wire. Node 1 is the observer; 2 and 3 join after it, so their hello
    // advertises are what it learns them from.
    sink_t at1, at3;
    named_sink_t at2;
    auto n1 = make_node(bus, 1, "/n1");
    n1->set_receiver(at1);
    auto n2 = make_node(bus, 2, "/n2");
    at2.bind(*n2);
    n2->bus()->set_peer_receiver(at2);
    auto n3 = make_node(bus, 3, "/n3");
    n3->set_receiver(at3);

    check(ok, n1->bus() != nullptr,
          "a CAN link exposes the bus facet; a point-to-point one does not");

    // The peers are synthesized from traffic. Nothing was registered, nothing was created.
    std::printf("who is audible on the wire:\n");
    tr::net::bus_link_t& facet = *n1->bus();
    check(ok, wait_for_peers(facet, 2, 2s), "node 1 heard both of the nodes that joined after it");
    const auto names = audible(facet);
    const bool has_two = std::find(names.begin(), names.end(), "n2") != names.end();
    const bool has_three = std::find(names.begin(), names.end(), "n3") != names.end();
    check(ok, has_two && has_three, "…named n2 and n3, derived from their CAN node ids");
    check(ok, std::find(names.begin(), names.end(), "n1") == names.end(),
          "…and never itself: the table is who I HEARD, not who is here");

    // A name that no node on this bus spells resolves to nothing — the enumeration is the
    // authority, and there is no fallback that would invent a route.
    check(ok, facet.peer_link("n9") == nullptr, "an unheard peer name resolves to no endpoint");
    check(ok, facet.peer_link("bogus") == nullptr, "a non-canonical name resolves to no endpoint");

    // A DIRECTED send on a broadcast medium: every node's link sees the CAN frames, but the
    // advertise names node 2, so only node 2 reassembles a frame out of them.
    std::printf("a directed send, on a medium that broadcasts:\n");
    tr::net::transport_t* to_n2 = facet.peer_link("n2");
    check(ok, to_n2 != nullptr, "peer_link('n2') resolved a directed endpoint");
    const auto payload = frame_of(20, 0x10);
    if (to_n2 != nullptr) to_n2->send(payload);
    check(ok, at2.wait_for(1, 2s), "node 2 delivered the frame");
    check(ok, at2.count() == 1 && at2.at(0) == payload,
          "…byte-exact, reassembled from 8-byte data fields");
    check(ok, at3.count() == 0, "node 3 heard the frames and delivered NOTHING — not addressed");

    // The inbound side of the same identity: node 2 named its sender from the handle the
    // delivery carried — a pure function of the CAN node id, so no lookup, no lock, and no
    // per-request state on either side of the exchange.
    check(ok, at2.count() == 1 && at2.sender(0) == "n1",
          "node 2 resolved the sender's name from the frame's own CAN id");

    std::printf("can: %zu peers audible to n1, 1 directed frame, %zu nodes that ignored it\n",
                names.size(), std::size_t{1});
    return ok ? 0 : 1;
}
