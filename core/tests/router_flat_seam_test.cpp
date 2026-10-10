// SPDX-License-Identifier: Apache-2.0
//
/**
 * @file
 * @brief #1582 — the router's peer-reachable `view::over_bytes` copies draw from its injected
 *        `flat` seam, never from the global heap; #1714 — the WARM COMPACT delivery makes no
 *        such copy at all, and draws ONE block from the graph's own source.
 *
 * ## What is being defended
 *
 * `docs/reference/09-memory-substrate.md`'s channel ledger says `fwd_router_t` keeps its OWN
 * memory seams — "receiver-pays, not an omission": a peer-driven receive path that exhausts
 * must not be able to reach the process heap the graph's write path lives on. Four ownership
 * copies in the router contradicted that sentence by taking `view::over_bytes`'s
 * single-argument (global-heap) overload while the router already held `flat` for exactly
 * this purpose:
 *
 * 1. the WARM COMPACT delivery's payload copy (`on_compact`, memoized terminus arm);
 * 2. the COLD COMPACT delivery's payload copy (`deliver_local`);
 * 3. + 4. the host-local subscribe door's return-route PATH TLV and SUBSCRIBER TLV
 *    (`subscribe_toward`).
 *
 * The first two are reachable by any peer holding a bound label, behind no ACL, at any
 * payload size the frame admits — an unbounded heap draw on a receive thread.
 *
 * #1714 then retired copy 1: the warm arm stores the payload the way the full-route terminus's
 * copy arm does, as ONE inline `value_t` drawn from the graph's injected source and adopted by
 * the store — one block where the `flat` segment plus the store's `value_t` were two. It is
 * still bounded (an injected, nothrow source) and still answered by value on exhaustion.
 *
 * ## What the tests assert
 *
 * - Each `flat` path charges `flat` for exactly the copies it makes, and lands.
 * - The warm COMPACT draws exactly ONE block in all — from the graph's source, none from `flat`.
 * - A refusing seam makes each path answer BY VALUE — a counted drop
 *   (`graph_t::delivery_drops().out_of_memory`) for the two deliveries, `BACKPRESSURE` for the
 *   subscribe — and nothing lands, which is the proof there is no heap fallback: a fallback
 *   would have delivered.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"
#include "route_frame_builder.hpp"  // host-only frame builders (#1779)
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::type_t;

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

/**
 * @brief A pass-through block source that counts what it served and can be told to refuse —
 *        injected as the GRAPH's source, so it sees every block the store draws.
 */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("test_counting_src") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse) {
            ++refused;
            return nullptr;
        }
        ++draws;
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }

    bool refuse = false; /**< @brief When set, every draw is refused (the exhaustion stand-in). */
    int draws = 0;       /**< @brief Draws served. */
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

/** @brief A PATH TLV over the given NAME segments. */
std::vector<std::byte> path_tlv(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A one-byte VALUE TLV — the COMPACT payload under test. */
std::vector<std::byte> value_tlv(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief The stored value's last payload byte, or nullopt when the vertex is empty. */
std::optional<std::uint8_t> stored_byte(graph_t& g, const char* p) {
    const auto v = g.find(path_t::parse(p)->key());
    if (!v) return std::nullopt;
    const auto r = g.read(*v);
    if (!r) return std::nullopt;
    const tr::view::view_t flat = (*r)->flatten();
    const std::span<const std::byte> b = flat.bytes();
    if (b.size() < 3) return std::nullopt;
    return static_cast<std::uint8_t>(b[b.size() - 1]);
}

/** @brief A view over fresh owned bytes — a producer's write value. */
tr::view::view_t owned(std::initializer_list<std::uint8_t> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::size_t i = 0;
    for (const std::uint8_t b : bytes) seg->bytes[i++] = std::byte{b};
    return tr::view::view_t::over(std::move(seg));
}

// --- COMPACT deliveries: cold (`deliver_local`) then warm (`on_compact`) ------------------

void compact_deliveries_charge_their_seams() {
    std::printf("COMPACT: the cold copy charges `flat`; the warm delivery is ONE graph block:\n");
    counting_source_t src;
    graph_t g(src);
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    counting_backend_t flat("flat");
    fwd_router_t router(g, {.flat = &flat});
    rec_link_t up;
    (void)router.attach_link("net/ws-client/up", up);
    router.on_frame("net/ws-client/up", tr::net::encode_advertise(5, path_tlv({"sink"})));
    check(flat.allocs == 0, "binding a label copies no payload — nothing charged yet");

    // COLD: the route is resolved and memoized; the payload copy is `deliver_local`'s.
    router.on_frame("net/ws-client/up", tr::net::encode_compact(5, value_tlv(1)));
    check(stored_byte(g, "/sink") == 1, "the cold COMPACT landed");
    check(flat.allocs == 1, "…and its ONE payload copy was drawn from `flat`");

    // WARM: the memoized handle is written through, and the payload is stored the way the
    // full-route terminus's copy arm stores it — one inline value the store adopts (#1714).
    const int graph_before = src.draws;
    router.on_frame("net/ws-client/up", tr::net::encode_compact(5, value_tlv(2)));
    check(stored_byte(g, "/sink") == 2, "the warm COMPACT landed");
    check(flat.allocs == 1, "…drawing NOTHING from `flat` (no separate payload segment)");
    check(src.draws - graph_before == 1,
          "…and exactly ONE block from the graph's injected source — the stored value itself");
    check(flat.refused == 0 && src.refused == 0, "nothing was refused on the positive control");
}

void a_refused_warm_compact_is_a_counted_drop() {
    std::printf("COMPACT warm: a refusing graph source is a counted drop, no heap fallback:\n");
    counting_source_t src;
    graph_t g(src);
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    counting_backend_t flat("flat");
    fwd_router_t router(g, {.flat = &flat});
    rec_link_t up;
    (void)router.attach_link("net/ws-client/up", up);
    router.on_frame("net/ws-client/up", tr::net::encode_advertise(5, path_tlv({"sink"})));
    router.on_frame("net/ws-client/up", tr::net::encode_compact(5, value_tlv(1)));
    check(stored_byte(g, "/sink") == 1, "precondition: the binding is warm");
    const std::uint64_t before = g.delivery_drops().out_of_memory;

    src.refuse = true;
    router.on_frame("net/ws-client/up", tr::net::encode_compact(5, value_tlv(9)));
    src.refuse = false;
    check(stored_byte(g, "/sink") == 1,
          "the refused delivery did NOT land — a heap fallback would have delivered 9");
    check(src.refused == 1, "the refusal was seen at the seam, on the ONE draw the arm makes");
    check(g.delivery_drops().out_of_memory == before + 1,
          "…and counted as ONE out-of-memory delivery drop");
    check(up.sent() == 0, "no NACK travelled back — the binding is intact, only the copy failed");

    // The seam serving again is enough: no state was poisoned by the refusal.
    router.on_frame("net/ws-client/up", tr::net::encode_compact(5, value_tlv(3)));
    check(stored_byte(g, "/sink") == 3, "the next COMPACT lands once the source serves again");
}

void a_refused_cold_compact_is_a_counted_drop() {
    std::printf("COMPACT cold: a refusing `flat` is a counted drop, no heap fallback:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    counting_backend_t flat("flat");
    fwd_router_t router(g, {.flat = &flat});
    rec_link_t up;
    (void)router.attach_link("net/ws-client/up", up);
    router.on_frame("net/ws-client/up", tr::net::encode_advertise(5, path_tlv({"sink"})));

    flat.refuse = true;
    router.on_frame("net/ws-client/up", tr::net::encode_compact(5, value_tlv(9)));
    check(!stored_byte(g, "/sink").has_value(),
          "the refused cold delivery did NOT land — a heap fallback would have delivered");
    check(flat.refused == 1, "the refusal was seen at the seam");
    check(g.delivery_drops().out_of_memory == 1, "…and counted as ONE out-of-memory drop");
    check(flat.allocs == 0, "nothing else was drawn on the way to the refusal");
}

// --- the host-local subscribe door -------------------------------------------------------

void subscribe_toward_charges_flat() {
    std::printf("subscribe_toward: the route and SUBSCRIBER TLVs charge `flat`:\n");
    graph_t g;
    counting_backend_t flat("flat");
    fwd_router_t router(g, {.flat = &flat});
    rec_link_t b;
    (void)router.attach_link("net/ws-client/b", b);
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
    (void)router.attach_link("net/ws-client/b", b);
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
    compact_deliveries_charge_their_seams();
    a_refused_warm_compact_is_a_counted_drop();
    a_refused_cold_compact_is_a_counted_drop();
    subscribe_toward_charges_flat();
    a_refused_subscribe_toward_answers_backpressure();
    return tr::testing::summary("router_flat_seam");
}
