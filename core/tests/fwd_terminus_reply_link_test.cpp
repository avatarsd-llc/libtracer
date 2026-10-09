// SPDX-License-Identifier: Apache-2.0
//
/**
 * @file
 * @brief #1709 — a terminus reply leaves through the link the request arrived on, with no
 *        by-name lookup, whenever that link is point-to-point.
 *
 * ## What is being defended
 *
 * A frame that arrives through a registered child's receiver carries that child's receive
 * context down to the terminus, and the context already holds the transport. The reply used to
 * find its egress again by scanning the child registry for the inbound NAME — a linear scan
 * per answered request, over links the router had in hand. Point-to-point replies now leave
 * through the context's link; only a BUS frame, whose inbound name is a PEER's, still resolves
 * by name (ADR-0044's peer fallback), and so does a frame pushed through the public by-name
 * `on_frame` door, which carries no context at all.
 *
 * ## What the tests assert
 *
 * - The INSTRUMENT is live: `fwd_router_t::reply_name_lookups` moves for a frame with no
 *   receive context, so a zero below is a measurement and not a dead counter.
 * - Ingress through a FLAT fake link, on both receive tiers (borrowed span and owning rope):
 *   exactly one reply, on that link, addressed to the request's `src` — and zero name lookups.
 * - The memory-refusal answer (#1612) takes the same inbound link, also with zero lookups.
 * - A BUS link keeps its peer resolution: the reply reaches the directed PEER endpoint, never
 *   the bus's own broadcasting `send`, and it is one by-name lookup. Run only when the build
 *   binds `tr::net::kBusLinks`; a bus-closed build prints a SKIP for that one case.
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;

/**
 * @brief A point-to-point link that records every frame the router sends down it, and can
 *        push a frame up its own receiver — the ingress a real transport drives.
 */
class record_link_t : public transport_t {
   public:
    explicit record_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    /** @brief Push @p frame up the span-tier receiver, borrowed. */
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    /** @brief Push @p frame up the rope-tier receiver as a two-link rope. */
    void inject_rope(std::span<const std::byte> frame) {
        tr::view::rope_t r;
        const std::size_t half = frame.size() / 2;
        for (const std::span<const std::byte> part : {frame.first(half), frame.subspan(half)}) {
            tr::view::segment_ptr_t seg = tr::view::heap_alloc(part.size());
            std::memcpy(seg->bytes.data(), part.data(), part.size());
            r.append(tr::view::view_t::over(std::move(seg)));
        }
        rx_.deliver_rope(std::move(r));
    }
    std::vector<std::vector<std::byte>> sent; /**< @brief Every frame handed down, in order. */

   private:
    bool ropes_ = false;
};

/** @brief A bus with one fixed peer, `n7`, whose directed endpoint is @ref peer. */
struct bus_record_link_t : transport_t, tr::net::bus_link_t {
    record_link_t peer;         /**< @brief The directed endpoint `peer_link("n7")` hands out. */
    std::size_t broadcasts = 0; /**< @brief Frames sent on the bus itself — a broadcast. */
    void send(std::span<const std::byte>) override { ++broadcasts; }
    void send(std::span<const std::span<const std::byte>>) override { ++broadcasts; }
    tr::net::bus_link_t* bus() override { return this; }
    transport_t* peer_link(std::string_view name) override {
        return name == "n7" ? &peer : nullptr;
    }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t& visit) const override {
        visit("n7");
    }
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t h,
                                             std::span<char>) const override {
        return h.valid() && h.index == 0 ? std::string_view{"n7"} : std::string_view{};
    }
    /** @brief Push @p frame up the peer-named slot, tagged as peer `n7`. */
    void inject(std::span<const std::byte> frame) {
        peer_rx_.deliver_borrowed(tr::net::peer_handle_t{0, 1}, frame);
    }
};

/** @brief Is @p frame a `FWD{REPLY}` whose `dst` is byte-for-byte @p req_src? */
bool is_reply_to(std::span<const std::byte> frame, std::span<const std::byte> req_src) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD || dec->children.size() < 2) return false;
    const auto& op = dec->children[0];
    if (op.type != type_t::VALUE || op.payload.size() != 1 ||
        std::to_integer<std::uint8_t>(op.payload[0]) != std::to_underlying(fwd_op_t::REPLY))
        return false;
    return std::ranges::equal(tr::wire::encode(dec->children[1]), req_src);
}

/** @brief A graph with one stored vertex, `/sensor/temp`, for a READ to land on. */
void seed(graph_t& g) {
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
}

/** @brief The instrument: a frame with no receive context is answered by name, and counted. */
void test_contextless_door_counts_a_lookup() {
    std::printf("the public by-name door has no ctx, so its reply is one counted lookup:\n");
    graph_t g;
    seed(g);
    fwd_router_t router(g);
    record_link_t client;
    (void)router.attach_link("client", client);

    const auto src = b_path({"client", "back"});
    router.on_frame("client", b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), src));
    check(client.sent.size() == 1 && is_reply_to(client.sent[0], src),
          "control: the READ is answered on the named link");
    check(router.reply_name_lookups() == 1, "instrument: the by-name reply was counted");
}

/** @brief Ingress through a flat link on the span tier: reply on that link, zero lookups. */
void test_flat_span_reply_leaves_on_inbound_link() {
    std::printf("a flat span-tier request is answered on its inbound link, no name lookup:\n");
    graph_t g;
    seed(g);
    fwd_router_t router(g);
    record_link_t other;
    record_link_t client;
    (void)router.attach_link("other", other);
    (void)router.attach_link("client", client);

    const auto src = b_path({"client", "back"});
    client.inject(b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), src));
    check(client.sent.size() == 1, "exactly one frame went back on the inbound link");
    check(!client.sent.empty() && is_reply_to(client.sent[0], src),
          "...and it is the FWD{REPLY} addressed to the request's src");
    check(other.sent.empty(), "no other link saw it");
    check(router.reply_name_lookups() == 0, "the reply took ZERO by-name lookups");
}

/** @brief The rope-tier twin: an owning multi-link frame takes the same inbound link. */
void test_flat_rope_reply_leaves_on_inbound_link() {
    std::printf("a flat rope-tier request is answered on its inbound link, no name lookup:\n");
    graph_t g;
    seed(g);
    fwd_router_t router(g);
    record_link_t client(/*ropes=*/true);
    (void)router.attach_link("client", client);

    const auto src = b_path({"client", "back"});
    client.inject_rope(b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), src));
    check(client.sent.size() == 1 && is_reply_to(client.sent[0], src),
          "one FWD{REPLY} to the request's src went back on the inbound link");
    check(router.reply_name_lookups() == 0, "the reply took ZERO by-name lookups");
}

/** @brief A source that refuses every draw, so the terminus takes its refusal arm. */
class refusing_source_t final : public tr::mem::block_source_t {
   public:
    refusing_source_t() noexcept : block_source_t("test_refusing_src") {}
    [[nodiscard]] void* try_alloc(std::size_t, std::size_t) noexcept override { return nullptr; }
    void release(void*, std::size_t, std::size_t) noexcept override {}
};

/** @brief The #1612 refusal answer takes the inbound link too. */
void test_refusal_answer_leaves_on_inbound_link() {
    std::printf("the memory-refusal answer is sent on the inbound link, no name lookup:\n");
    graph_t g;
    seed(g);
    refusing_source_t rx;
    fwd_router_t router(g, {.rx = &rx});
    record_link_t client;
    (void)router.attach_link("client", client);

    const auto src = b_path({"client", "back"});
    client.inject(b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), src));
    check(router.drop_stats().arena_dropped == 1, "instrument: the decode arena was refused");
    check(client.sent.size() == 1 && is_reply_to(client.sent[0], src),
          "the refusal was answered on the inbound link");
    check(router.reply_name_lookups() == 0, "the answer took ZERO by-name lookups");
}

/** @brief A bus keeps its peer resolution: the reply reaches the directed peer endpoint. */
void test_bus_reply_keeps_peer_resolution() {
    std::printf("a bus request is still answered through the peer's directed endpoint:\n");
    graph_t g;
    seed(g);
    fwd_router_t router(g);
    bus_record_link_t bus;
    (void)router.attach_link("bus", bus);

    const auto src = b_path({"n7", "back"});
    bus.inject(b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), src));
    check(bus.peer.sent.size() == 1 && is_reply_to(bus.peer.sent[0], src),
          "one FWD{REPLY} reached peer n7's directed endpoint");
    check(bus.broadcasts == 0, "nothing was broadcast on the bus itself");
    check(router.reply_name_lookups() == 1, "the bus reply resolved the peer by name, once");
}

}  // namespace

int main() {
    test_contextless_door_counts_a_lookup();
    test_flat_span_reply_leaves_on_inbound_link();
    test_flat_rope_reply_leaves_on_inbound_link();
    test_refusal_answer_leaves_on_inbound_link();
    if constexpr (tr::net::kBusLinks) {
        test_bus_reply_keeps_peer_resolution();
    } else {
        // The one case here that needs the ADR-0044 bus module PRESENT: under
        // `kBusLinks = false` the router is told `bus_of` is nullptr and mounts the bus as a
        // point-to-point child, so there is no peer endpoint for the reply to reach. The other
        // cases are tier-blind and the point-to-point tier is exactly what a bus-closed node
        // runs, so the target is not `bus`-labelled (the link_token_carry_test precedent).
        std::printf("bus reply SKIPPED: this build closed the ADR-0044 bus module out\n");
    }
    return tr::testing::summary("fwd_terminus_reply_link");
}
