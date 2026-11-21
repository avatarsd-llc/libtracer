// SPDX-License-Identifier: Apache-2.0
//
/**
 * @file
 * @brief The SUBSCRIPTION-RETAINED seam (#1610, #2875): what a remote SUBSCRIBE keeps for the
 *        life of the subscription is ONE block, drawn from a source of the host's choosing.
 *
 * ## What is being defended
 *
 * A remote SUBSCRIBE keeps the source `SUBSCRIBER` TLV, "the ONE route copy of the
 * subscription's life" (ADR-0041 §2) and the completed reverse chain until the subscription
 * dies. Until #1610 they came from `flat`, the backend that also serves every per-operation
 * rope flatten on the forward and terminus paths. Until #2875 they were two or three segments,
 * and a router that was not given the seam took them from `flat`, outside the long-lived link
 * plane a bounded node sizes and counts.
 *
 * Sharing `flat` is not a correctness problem — it is a SIZING one, and it is the same sizing
 * problem that made `egress` a dedicated injection. `flat` is documented and sized against
 * per-operation FLATTEN bytes, whose live set is bounded by the threads that can be inside the
 * router. This block is neither per-operation nor a flatten: its live set is the SUBSCRIPTION
 * POPULATION.
 *
 * ## What the tests assert
 *
 * 1. Injected, the retained block lands on `retained` — ONE draw — and nothing on `flat`.
 * 2. Un-injected on a bare resolver, it lands on `flat`.
 * 3. The seam is only for RETENTION: an ordinary WRITE's flatten still comes from `flat`
 *    even when `retained` is injected, so a host cannot accidentally re-scope the hot path.
 * 4. A `retained` that refuses answers by value — the subscribe does not bind, nothing
 *    aborts, and nothing falls back to another source.
 * 5. Through a router that was not given `retained`, the block is drawn from the router's
 *    `link_src` and held there for the life of the subscription; every other draw of the
 *    subscribe lands on an injected plane, and none on `flat`.
 */
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief A pass-through backend that counts what it served. */
class counting_backend_t final : public tr::mem::mem_backend_t {
   public:
    explicit counting_backend_t(const char* name, bool refuse = false) noexcept
        : mem_backend_t(name), refuse_(refuse) {}

    tr::view::segment_t* alloc(std::size_t size,
                               tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        if (refuse_) {
            ++refused;
            return nullptr;
        }
        ++allocs;
        return tr::mem::heap_backend().alloc(size, hint);
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        tr::mem::heap_backend().destroy(seg);
    }
    void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse_) {
            ++refused;
            return nullptr;
        }
        ++allocs;
        ++live;
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        --live;
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::size_t alignment() const noexcept override { return tr::mem::heap_backend().alignment(); }
    std::size_t max_segment_size() const noexcept override {
        return tr::mem::heap_backend().max_segment_size();
    }
    tr::mem::mem_space_t space() const noexcept override { return tr::mem::heap_backend().space(); }

    int allocs = 0;
    int refused = 0;
    int live = 0; /**< @brief Raw blocks drawn and not yet released. */

   private:
    bool refuse_;
};

/** @brief The remote-subscribe frame: WRITE to `/x:subscribers[]` carrying a SUBSCRIBER TLV. */
std::vector<std::byte> subscribe_fwd() {
    std::vector<std::byte> field_body;
    tr::wire::emit_name(field_body, "subscribers");
    const std::byte mode[1] = {std::byte{1}};  // index_mode ELEMENT (append)
    tr::wire::emit_tlv(field_body, type_t::VALUE, opt_t{}, mode);
    std::vector<std::byte> field;
    tr::wire::emit_tlv(field, type_t::FIELD, opt_t{.pl = true}, field_body);

    std::vector<std::byte> sub;
    tr::wire::emit_tlv(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({"sink"}));

    return b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"ret"}), field, sub);
}

/** @brief A plain WRITE of a scalar VALUE — the per-operation flatten the seam must NOT take. */
std::vector<std::byte> write_fwd() {
    std::vector<std::byte> payload;
    const std::byte v[4] = {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, v);
    return b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"ret"}), {}, payload);
}

tr::graph::result_t<tr::view::rope_t> resolve_bytes(op_resolver_t& resolver,
                                                    std::span<const std::byte> fwd,
                                                    std::string_view inbound_link) {
    const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
    if (!arena) return std::unexpected(tr::graph::status_t::INVALID_PATH);
    return resolver.resolve(*arena, inbound_link);
}

/** @brief A graph with `/x` and `/sink` registered — producer and subscription target. */
void build(tr::graph::graph_t& g) {
    (void)g.register_vertex(path_t("/x"), tr::graph::role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sink"), tr::graph::role_t::STORED_VALUE);
}

void injected_retention_leaves_flat_untouched() {
    std::printf("injected: the retained block lands on `retained`, never on `flat`:\n");
    tr::graph::graph_t g;
    build(g);
    counting_backend_t flat("flat");
    counting_backend_t egress("egress");
    counting_backend_t retained("retained");
    op_resolver_t resolver(g, &flat, &egress, &retained);

    const auto reply = resolve_bytes(resolver, subscribe_fwd(), "link-a");
    tr::testing::check(reply.has_value(), "the remote subscribe resolves");

    // The SUBSCRIBER TLV and the return route — one block, holding both.
    tr::testing::check(retained.allocs == 1,
                       "`retained` served ONE block: the SUBSCRIBER TLV and the ONE route copy");
    tr::testing::check(flat.allocs == 0,
                       "`flat` served NOTHING — which is the whole point: a size-classed flat "
                       "front keeps its slots for the churn it was cut for");
}

void uninjected_retention_still_comes_from_flat() {
    std::printf("un-injected bare resolver: the block comes from `flat`:\n");
    tr::graph::graph_t g;
    build(g);
    counting_backend_t flat("flat");
    counting_backend_t egress("egress");
    op_resolver_t resolver(g, &flat, &egress);  // no `retained`

    const auto reply = resolve_bytes(resolver, subscribe_fwd(), "link-a");
    tr::testing::check(reply.has_value(), "the remote subscribe resolves");
    tr::testing::check(flat.allocs == 1,
                       "the default is not the heap, it is wherever flattens already came from");
}

void the_seam_is_for_retention_only() {
    std::printf("scope: an ordinary WRITE's copy never comes from `retained`:\n");
    tr::graph::graph_t g;
    build(g);
    counting_backend_t flat("flat");
    counting_backend_t egress("egress");
    counting_backend_t retained("retained");
    op_resolver_t resolver(g, &flat, &egress, &retained);

    const auto reply = resolve_bytes(resolver, write_fwd(), "link-a");
    tr::testing::check(reply.has_value(), "the write resolves");
    tr::testing::check(retained.allocs == 0,
                       "`retained` is for SUBSCRIPTION-scoped allocations only — a host cannot "
                       "re-scope the hot path by injecting it");
    // A contiguous WRITE below the share threshold copies into the value's own block, drawn
    // from the GRAPH's source (RFC-0028 §5.1) — so neither router seam is asked at all.
    tr::testing::check(flat.allocs == 0,
                       "…and its ownership copy is the value's own block, not a `flat` draw");
}

void a_refusing_retained_seam_answers_by_value() {
    std::printf("exhaustion: a refused retention does not bind, and does not abort:\n");
    tr::graph::graph_t g;
    build(g);
    counting_backend_t flat("flat");
    counting_backend_t egress("egress");
    counting_backend_t retained("retained", /*refuse=*/true);
    op_resolver_t resolver(g, &flat, &egress, &retained);

    const auto reply = resolve_bytes(resolver, subscribe_fwd(), "link-a");
    tr::testing::check(reply.has_value(), "exhaustion is a REPLY, never an abort or a throw");
    tr::testing::check(retained.refused > 0, "…and the refusal was seen at the seam");
    tr::testing::check(flat.allocs == 0,
                       "a refused retention does not silently fall back to `flat` — that would "
                       "be the bound this seam exists to express, quietly unbound");
}

/** @brief A link that drops what it is sent; the test injects frames through its receiver. */
class null_link_t final : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte>) override {}
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
};

void a_router_draws_the_block_from_its_link_plane() {
    std::printf("router: the block is the link plane's, for the life of the subscription:\n");
    counting_backend_t graph_src("graph");
    tr::graph::graph_t g(graph_src);
    const tr::graph::vertex_handle_t x =
        g.register_vertex(path_t("/x"), tr::graph::role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sink"), tr::graph::role_t::STORED_VALUE);
    counting_backend_t link_src("link_src");
    counting_backend_t rx("rx");
    counting_backend_t flat("flat");
    counting_backend_t egress("egress");
    tr::net::fwd_router_t router(
        g, {.link_src = &link_src, .rx = &rx, .flat = &flat, .egress = &egress});
    null_link_t link;
    tr::testing::check(router.attach_link("link-a", link), "the link registers");

    const int held = link_src.live;
    link.inject(subscribe_fwd());
    tr::testing::check(g.has_subscribers(x), "the remote subscribe binds");
    tr::testing::check(link_src.live == held + 1,
                       "the subscription holds ONE block of the router's link plane");
    tr::testing::check(flat.allocs == 0, "`flat` served nothing for the subscription");
    tr::testing::check(rx.live == 0 && egress.live == 0,
                       "the per-frame planes hold nothing once the subscribe is answered");

    (void)router.remove_child("link-a");
    tr::testing::check(link_src.live < held + 1,
                       "the block goes back to the link plane with the subscription");
}

}  // namespace

int main() {
    injected_retention_leaves_flat_untouched();
    uninjected_retention_still_comes_from_flat();
    the_seam_is_for_retention_only();
    a_refusing_retained_seam_answers_by_value();
    a_router_draws_the_block_from_its_link_plane();
    return tr::testing::summary("retained_subscription_seam");
}
