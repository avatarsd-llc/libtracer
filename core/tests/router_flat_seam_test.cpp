// SPDX-License-Identifier: Apache-2.0
//
/**
 * @file
 * @brief #1582 — the router's host-local subscribe door draws its `view::over_bytes` copies
 *        from its injected `flat` seam, never from the global heap.
 *
 * ## What is being defended
 *
 * `docs/reference/09-memory-substrate.md`'s channel ledger says `fwd_router_t` keeps its OWN
 * memory seams — "receiver-pays, not an omission". The host-local subscribe door
 * (`subscribe_toward`) makes two ownership copies, its return-route PATH TLV and its
 * SUBSCRIBER TLV, and both draw from `flat`. The two COMPACT delivery copies #1582 also
 * covered are retired with COMPACT itself (#1951).
 *
 * ## What the tests assert
 *
 * - The door charges `flat` for exactly the copies it makes, and the bind lands.
 * - A refusing seam answers `BACKPRESSURE` by value and nothing binds, which is the proof
 *   there is no heap fallback: a fallback would have bound.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::net::fwd_router_t;

using tr::testing::check;

/** @brief A pass-through backend that counts what it served and can be told to refuse. */
class counting_backend_t final : public tr::mem::mem_backend_t {
   public:
    explicit counting_backend_t(const char* name) noexcept : mem_backend_t(name) {}

    tr::view::segment_t* alloc(std::size_t size,
                               tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        if (refuse) {
            ++refused;
            return nullptr;
        }
        ++allocs;
        return tr::mem::heap_backend().alloc(size, hint);
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        tr::mem::heap_backend().destroy(seg);
    }
    std::size_t alignment() const noexcept override { return tr::mem::heap_backend().alignment(); }
    std::size_t max_segment_size() const noexcept override {
        return tr::mem::heap_backend().max_segment_size();
    }
    tr::mem::mem_space_t space() const noexcept override { return tr::mem::heap_backend().space(); }

    bool refuse = false; /**< @brief When set, every draw is refused (the exhaustion stand-in). */
    int allocs = 0;      /**< @brief Draws served. */
    int refused = 0;     /**< @brief Draws refused. */
};

/** @brief A link that records the frames it is asked to send. */
class rec_link_t final : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> f) override { sent_.emplace_back(f.begin(), f.end()); }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte> whole;
        for (const auto s : iov) whole.insert(whole.end(), s.begin(), s.end());
        sent_.push_back(std::move(whole));
    }
    [[nodiscard]] std::size_t sent() const noexcept { return sent_.size(); }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

/** @brief A view over fresh owned bytes — a producer's write value. */
tr::view::view_t owned(std::initializer_list<std::uint8_t> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::size_t i = 0;
    for (const std::uint8_t b : bytes) seg->bytes[i++] = std::byte{b};
    return tr::view::view_t::over(std::move(seg));
}

// --- the host-local subscribe door -------------------------------------------------------

void subscribe_toward_charges_flat() {
    std::printf("subscribe_toward: the route and SUBSCRIBER TLVs charge `flat`:\n");
    graph_t g;
    counting_backend_t flat("flat");
    fwd_router_t router(g, {.flat = &flat});
    rec_link_t b;
    (void)router.add_child("net/ws-client/b", b);
    (void)g.register_vertex(path_t("/light/rgb"), role_t::STORED_VALUE);

    const auto s =
        router.subscribe_toward(path_t("/light/rgb"), path_t("/net/ws-client/b/display/val"));
    check(s.has_value(), "the bind admits the subscription");
    check(flat.allocs == 2, "the route PATH TLV and the SUBSCRIBER TLV — two, and exactly two");
    check(flat.refused == 0, "nothing was refused on the positive control");

    (void)g.write(path_t("/light/rgb"), owned({0x01, 0x00, 0x01, 0x00, 0x2A}));
    check(b.sent() == 1, "a producer write reaches the bound link");
}

void a_refused_subscribe_toward_answers_backpressure() {
    std::printf("subscribe_toward: a refusing `flat` is BACKPRESSURE by value, no bind:\n");
    graph_t g;
    counting_backend_t flat("flat");
    fwd_router_t router(g, {.flat = &flat});
    rec_link_t b;
    (void)router.add_child("net/ws-client/b", b);
    (void)g.register_vertex(path_t("/light/rgb"), role_t::STORED_VALUE);

    flat.refuse = true;
    const auto s =
        router.subscribe_toward(path_t("/light/rgb"), path_t("/net/ws-client/b/display/val"));
    check(!s.has_value() && s.error() == status_t::BACKPRESSURE,
          "exhaustion is BACKPRESSURE by value — never a throw, never a heap fallback");
    check(flat.refused == 1, "the FIRST refusal ends the door — the second copy is never tried");
    check(flat.allocs == 0, "nothing was drawn");

    (void)g.write(path_t("/light/rgb"), owned({0x01, 0x00, 0x01, 0x00, 0x2A}));
    check(b.sent() == 0, "a refused bind is no bind: the producer write reaches no link");
}

}  // namespace

int main() {
    subscribe_toward_charges_flat();
    a_refused_subscribe_toward_answers_backpressure();
    return tr::testing::summary("router_flat_seam");
}
