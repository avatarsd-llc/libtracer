// SPDX-License-Identifier: Apache-2.0
//
/**
 * @file
 * @brief The SUBSCRIPTION-RETAINED seam (#1610): the two allocations a remote SUBSCRIBE keeps
 *        for the life of the subscription can be taken from a backend of the host's choosing.
 *
 * ## What is being defended
 *
 * A remote SUBSCRIBE keeps two allocations until the subscription dies — the source
 * `SUBSCRIBER` TLV and "the ONE route copy of the subscription's life" (ADR-0041 §2). Until
 * this seam existed, both came from `flat`, the backend that also serves every per-operation
 * rope flatten on the forward and terminus paths.
 *
 * Sharing one backend between them is not a correctness problem — it is a SIZING one, and it
 * is the same sizing problem that made `egress` a dedicated injection. `flat` is documented
 * and sized against per-operation FLATTEN bytes, whose live set is bounded by the threads
 * that can be inside the router. These two are neither per-operation nor flattens: their
 * live set is the SUBSCRIPTION POPULATION. A host that size-classes `flat` therefore finds
 * the classes filled by a set that never returns, with nothing left for the churn they were
 * cut for — the classes stop working exactly when a client is attached.
 *
 * ## What the tests assert
 *
 * 1. Injected, the two retained allocations land on `retained` and NOT on `flat`.
 * 2. Un-injected, they land on `flat` exactly as before — the change is additive and an
 *    existing host is byte-unchanged.
 * 3. The seam is only for RETENTION: an ordinary WRITE's flatten still comes from `flat`
 *    even when `retained` is injected, so a host cannot accidentally re-scope the hot path.
 * 4. A `retained` that refuses answers by value — the subscribe does not bind, and nothing
 *    aborts.
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
    std::size_t alignment() const noexcept override { return tr::mem::heap_backend().alignment(); }
    std::size_t max_segment_size() const noexcept override {
        return tr::mem::heap_backend().max_segment_size();
    }
    tr::mem::mem_space_t space() const noexcept override { return tr::mem::heap_backend().space(); }

    int allocs = 0;
    int refused = 0;

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
    std::printf("injected: the retained pair lands on `retained`, never on `flat`:\n");
    tr::graph::graph_t g;
    build(g);
    counting_backend_t flat("flat");
    counting_backend_t egress("egress");
    counting_backend_t retained("retained");
    op_resolver_t resolver(g, &flat, &egress, &retained);

    const auto reply = resolve_bytes(resolver, subscribe_fwd(), "link-a");
    tr::testing::check(reply.has_value(), "the remote subscribe resolves");

    // The SUBSCRIBER TLV and the return route — two, and exactly two.
    tr::testing::check(retained.allocs == 2,
                       "`retained` served the SUBSCRIBER TLV and the ONE route copy");
    tr::testing::check(flat.allocs == 0,
                       "`flat` served NOTHING — which is the whole point: a size-classed flat "
                       "front keeps its slots for the churn it was cut for");
}

void uninjected_retention_still_comes_from_flat() {
    std::printf("un-injected: byte-unchanged — the pair still comes from `flat`:\n");
    tr::graph::graph_t g;
    build(g);
    counting_backend_t flat("flat");
    counting_backend_t egress("egress");
    op_resolver_t resolver(g, &flat, &egress);  // no `retained`

    const auto reply = resolve_bytes(resolver, subscribe_fwd(), "link-a");
    tr::testing::check(reply.has_value(), "the remote subscribe resolves");
    tr::testing::check(flat.allocs == 2,
                       "an existing host sees exactly what it saw before — the default is not "
                       "the heap, it is wherever flattens already came from");
}

void the_seam_is_for_retention_only() {
    std::printf("scope: an ordinary WRITE's flatten still comes from `flat`:\n");
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
    tr::testing::check(flat.allocs > 0, "…and the per-operation flatten still came from `flat`");
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

}  // namespace

int main() {
    injected_retention_leaves_flat_untouched();
    uninjected_retention_still_comes_from_flat();
    the_seam_is_for_retention_only();
    a_refusing_retained_seam_answers_by_value();
    return tr::testing::summary("retained_subscription_seam");
}
