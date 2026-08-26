/**
 * @file
 * @brief #981/#1570 — the `try_*` call sites that MIGRATED off the `-fno-exceptions` probe
 *        window really draw from the injected `block_source_t`, and answer its exhaustion
 *        by value.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #923 (PR #972) made `tr::detail::try_reserve` / `try_push_back` report growth failure by
 * value on every profile whose allocation THROWS. Under `-fno-exceptions` — the profile the
 * MCU ships — it cannot: the helper probes the global heap, frees the probe block, and then
 * runs the throwing `reserve` on the inference that the block is still free. A context
 * switch in that window lets another task take it and the `reserve` abort()s the node
 * (#850). #981 ruled the disposition per site: a site whose element type is trivially
 * copyable migrates to the ADR-0065 failable seam (`tr::mem::block_source_t` /
 * `block_array_t`, ONE refusable `try_alloc` per growth, no window); a `view_t` /
 * `std::vector` / `std::string`-element site keeps the helper and states the residual at the
 * site.
 *
 * Three sites migrated, and this file is their proof:
 *
 *   - `graph_t::read_subtree_folded`'s collect stack (`work_t` = a vertex pointer plus an
 *     index) — a peer picks which composed root to READ, so it picks how far this grows;
 *   - `fwd_router_t::deliver_remote`'s egress iov tables (`std::span`), both the
 *     reverse-list arm and the canonical full-route arm — one per remote delivery;
 *   - `fwd_router_t::resolve_terminus` / `::resolve_terminus_rope`'s REPLY iov tables
 *     (`std::span`), one per inbound request frame — the last migratable window on the
 *     reply path (#1570). A peer picks the reply's link count too: a composed-root read
 *     answers with as many links as it produced, which is why the table has no fixed
 *     bound. These two draw from the ROUTER's injected receive source rather than the
 *     graph's `ctl` — the charge belongs where the frame's own arena is charged.
 *
 * @section instrument Why neither check is vacuous
 *
 * A "soft-fails on exhaustion" assertion passes trivially if the code under test never
 * consults the source it is being starved of — exactly what the PRE-migration code does,
 * since it allocated from the GLOBAL heap and the injected `ctl` source saw nothing. So the
 * gate here is not "refuse everything": @ref gated_source_t refuses ONE exact block shape,
 * the `(elements * sizeof(T), alignof(T))` request the migrated container makes, and every
 * armed case asserts a POSITIVE instrument first — `served()` shows the source was really
 * asked for that shape. Revert any of the migrations and the shape is never requested: the
 * instrument check fails AND the operation succeeds where the test demands a refusal, so
 * both halves redden. (The whole-source arm is deliberately NOT used: a graph draws its
 * arena and label-table blocks from the same `ctl`, so blanket refusal could not tell the
 * iov table's failure from a decode's.)
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;
using tr::testing::make_value;
using tr::testing::detail::append;

/**
 * @brief A `block_source_t` that forwards to the platform heap but can REFUSE one exact
 *        block shape, and counts how often that shape was asked for.
 *
 * The shape (bytes + alignment) is the instrument: it names the migrated container's
 * growth precisely, so an armed refusal starves THAT allocation and nothing else, and
 * `served()` proves the site reached the seam at all.
 */
class gated_source_t final : public tr::mem::block_source_t {
   public:
    gated_source_t() noexcept : block_source_t("gated") {}

    /** @brief Watch (and, when @p refuse, deny) requests of exactly @p bytes / @p align. */
    void watch(std::size_t bytes, std::size_t align, bool refuse) noexcept {
        bytes_.store(bytes, std::memory_order_relaxed);
        align_.store(align, std::memory_order_relaxed);
        refuse_.store(refuse, std::memory_order_relaxed);
        served_.store(0, std::memory_order_relaxed);
        refused_.store(0, std::memory_order_relaxed);
    }
    /** @brief How many times the watched shape was requested and GRANTED. */
    [[nodiscard]] std::size_t served() const noexcept {
        return served_.load(std::memory_order_relaxed);
    }
    /** @brief How many times the watched shape was requested and DENIED. */
    [[nodiscard]] std::size_t refused() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (bytes == bytes_.load(std::memory_order_relaxed) &&
            align == align_.load(std::memory_order_relaxed)) {
            if (refuse_.load(std::memory_order_relaxed)) {
                refused_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            served_.fetch_add(1, std::memory_order_relaxed);
        }
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t /*bytes*/, std::size_t align) noexcept override {
        ::operator delete(p, std::align_val_t{align}, std::nothrow);
    }

   private:
    // Atomics rather than a mutex: try_alloc is called with graph/router locks held, so a
    // test-owned lock here hands TSan a cross-test lock-order cycle (map_mutex_ -> m_ in the
    // read path, m_ -> map_mutex_ via add_child). Relaxed is enough — every armed/asserted
    // transition happens-before the call it instruments on the test's own thread.
    std::atomic<std::size_t> bytes_{0};
    std::atomic<std::size_t> align_{0};
    std::atomic<bool> refuse_{false};
    std::atomic<std::size_t> served_{0};
    std::atomic<std::size_t> refused_{0};
};

/**
 * @brief The collect stack's element in `read_subtree_folded` — a vertex pointer plus its
 *        parent index. Mirrored here ONLY to compute the block shape the migrated
 *        `block_array_t` asks for; the production type is function-local.
 */
struct work_shape_t {
    void* v = nullptr;
    std::size_t parent = 0;
};

/**
 * @brief The composed read's NODE-TABLE element, mirrored for the same reason
 *        @ref work_shape_t is: to compute the block shape its first growth asks for.
 *
 * `snap_node_t` holds a `std::shared_ptr`, so it can never take `block_array_t`'s memcpy
 * relocation — which is why #873 phase 1 put it on a source ALLOCATOR instead: the element
 * type and its destructors are untouched and only the block moves onto the injected store.
 * The mirror must therefore carry a `shared_ptr` too, or the size is wrong.
 */
struct snap_shape_t {
    const void* v = nullptr;
    std::shared_ptr<const void> lkv;
    std::size_t parent = 0;
    std::size_t body_len = 0;
};

/**
 * @brief The node table's FIRST growth. `tr::detail::try_push_back` grows an empty vector to
 *        `grow_capacity(0) == 1`, so the first request is exactly one element.
 */
constexpr std::size_t kNodeTableBytes = sizeof(snap_shape_t);
constexpr std::size_t kNodeTableAlign = alignof(snap_shape_t);

/**
 * @brief `block_array_t`'s FIRST growth is 8 elements (`grow()`: `have < 4 ? 8 : have * 2`),
 *        so this is the exact block the collect stack requests on its very first push.
 */
constexpr std::size_t kStackBytes = 8 * sizeof(work_shape_t);
constexpr std::size_t kStackAlign = alignof(work_shape_t);

/** @brief The iov table is `reserve`d to its exact final count, so its ONE request is
 *         `(3 + links) * sizeof(std::span<const std::byte>)`. */
constexpr std::size_t iov_bytes(std::size_t links) {
    return (3 + links) * sizeof(std::span<const std::byte>);
}
constexpr std::size_t kIovAlign = alignof(std::span<const std::byte>);

// --- (a) the composed-read collect stack -------------------------------------

/**
 * @brief `read_subtree_folded`'s collect stack draws from the graph's injected `ctl` source,
 *        and its exhaustion is BACKPRESSURE — not an abort, not a partial rope.
 */
void test_collect_stack_on_the_seam() {
    std::printf("read_subtree_folded's collect stack (block_array_t over the injected ctl):\n");
    gated_source_t src;
    graph_t g(&src);

    const auto root_path = path_t::parse("/plant");
    auto root = g.register_vertex(*root_path, role_t::STORED_VALUE);
    for (std::string_view leaf : {"/plant/t", "/plant/h", "/plant/p"}) {
        const auto p = path_t::parse(leaf);
        auto v = g.register_vertex(*p, role_t::STORED_VALUE);
        tr::view::rope_t val;
        val.append(make_value({0x08, 0x00, 0x01, 0x00, 0x2A}));
        (void)g.write(v, std::move(val));
    }

    // Instrument: the read must ASK the seam for the stack block. On the pre-#981 code the
    // stack was a std::vector on the global heap and this count is 0.
    src.watch(kStackBytes, kStackAlign, false);
    const auto ok = g.read_subtree_folded(root, "peer");
    check(ok.has_value(), "the composed read succeeds with a permissive source");
    check(src.served() == 1, "the collect stack took its block from the INJECTED ctl source");

    // Armed: refuse exactly that block. The read must degrade by value.
    src.watch(kStackBytes, kStackAlign, true);
    const auto refused = g.read_subtree_folded(root, "peer");
    check(src.refused() >= 1, "the injector fired (an unreached try_alloc would be vacuous)");
    check(!refused.has_value() && refused.error() == tr::graph::status_t::BACKPRESSURE,
          "an exhausted collect stack answers BACKPRESSURE, never an abort");

    // And the graph is undamaged: disarm, read again.
    src.watch(0, 0, false);
    check(g.read_subtree_folded(root, "peer").has_value(),
          "the refused read left the graph intact — the next read succeeds");
}

// --- (b) the remote-delivery iov table ---------------------------------------

/** @brief A FIELD write appending to `:subscribers` — the wire door a remote subscribe uses. */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "subscribers");
    std::vector<std::byte> idx;
    const std::array<std::byte, 1> one{std::byte{1}};
    tr::wire::emit_tlv(idx, type_t::VALUE, opt_t{}, std::span<const std::byte>(one));
    append(body, idx);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief SUBSCRIBER{ PATH target, SETTINGS{ NAME "delivery_compact" VALUE 0 } }. */
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target) {
    std::vector<std::byte> body;
    append(body, target);
    std::vector<std::byte> qos;
    tr::wire::emit_name(qos, "delivery_compact");
    const std::array<std::byte, 1> zero{std::byte{0}};
    std::vector<std::byte> zv;
    tr::wire::emit_tlv(zv, type_t::VALUE, opt_t{}, std::span<const std::byte>(zero));
    append(qos, zv);
    std::vector<std::byte> settings;
    tr::wire::emit_tlv(settings, type_t::SETTINGS, opt_t{.pl = true}, qos);
    append(body, settings);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

/** @brief An in-memory transport that counts the frames the router sends it. */
class counting_link_t final : public transport_t {
   public:
    void send(std::span<const std::byte>) override {
        const std::lock_guard lock(m_);
        ++frames_;
    }
    void send(std::span<const std::span<const std::byte>>) override {
        const std::lock_guard lock(m_);
        ++frames_;
    }
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    /** @brief Push @p frame up the ROPE tier — the second terminus (#1570 case (c)). */
    void inject_rope(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
    std::size_t take() {
        const std::lock_guard lock(m_);
        return std::exchange(frames_, 0u);
    }

   private:
    std::mutex m_;
    std::size_t frames_ = 0;
};

/**
 * @brief `deliver_remote`'s full-route egress iov table draws from the graph's `ctl` source,
 *        and its exhaustion DROPS that one delivery — never an abort.
 */
void test_delivery_iov_on_the_seam() {
    std::printf("deliver_remote's egress iov table (block_array_t over the injected ctl):\n");
    gated_source_t src;
    graph_t g(&src);
    fwd_router_t router(g);
    counting_link_t link;
    (void)router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    auto v = g.register_vertex(*p, role_t::STORED_VALUE);
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}))));
    (void)link.take();  // discard the subscribe REPLY

    // A single-link value: the table is head + route + empty src + 1 payload span.
    const auto write_one = [&g, &v](std::uint32_t x) {
        tr::view::rope_t val;
        val.append(make_value({std::uint8_t{0x08}, 0x00, 0x04, 0x00,
                               static_cast<std::uint8_t>(x & 0xFFu), 0x00, 0x00, 0x00}));
        return g.write(v, std::move(val)).has_value();
    };

    // Instrument: the delivery must ASK the seam for the 4-span table. Pre-#981 that table
    // was a std::vector on the global heap and this count is 0.
    src.watch(iov_bytes(1), kIovAlign, false);
    check(write_one(1), "the write lands with a permissive source");
    check(src.served() == 1, "the egress iov table came from the INJECTED ctl source");
    check(link.take() == 1, "one delivery frame reached the subscriber");

    // Armed: refuse exactly that table. The delivery drops; the STORE still lands (the
    // fan-out is best-effort per RFC-0004 §D) and nothing aborts.
    src.watch(iov_bytes(1), kIovAlign, true);
    check(write_one(2), "the write still lands when the egress table cannot be allocated");
    check(src.refused() >= 1, "the injector fired (an unreached try_alloc would be vacuous)");
    check(link.take() == 0, "an exhausted iov table DROPS the delivery, never aborts");

    // Disarm: the very next write delivers again — the refusal left no residue.
    src.watch(iov_bytes(1), kIovAlign, false);
    check(write_one(3), "the write lands again");
    check(link.take() == 1, "delivery resumes once the source recovers");
}

// --- (c) the terminus REPLY iov table (#1570) --------------------------------

/** @brief The reply table is `reserve`d to the rope's exact link count, so its ONE request
 *         is `links * sizeof(std::span<const std::byte>)` — no `+3` head/route/src spans:
 *         the terminus sends the assembled reply rope and nothing beside it. */
constexpr std::size_t reply_iov_bytes(std::size_t links) {
    return links * sizeof(std::span<const std::byte>);
}
/**
 * @brief The link count of the reply this case drives: a `FWD{REPLY}` over a single-segment
 *        stored value assembles as TWO links — the resolver's freshly emitted head and the
 *        stored value's own segment, adopted by reference (ADR-0053 5, no flatten).
 *
 * MEASURED, not assumed. The gate names one exact block shape, so a wrong count here would
 * watch a shape nobody requests and the `served()` instrument would redden immediately —
 * which is precisely the vacuity the @ref instrument section above exists to prevent.
 */
constexpr std::size_t kReplyLinks = 2;

/**
 * @brief `resolve_terminus`'s REPLY iov table draws from the router's injected RECEIVE
 *        source, and its exhaustion drops the reply — never an abort, at any reply size.
 *
 * The third migrated site (#1570) and the last one on the reply path. Both termini built
 * this table with `rope_t::try_to_iovec` — a `std::vector` on the GLOBAL heap, grown through
 * `tr::detail::try_reserve`'s `-fno-exceptions` probe window — once per inbound request
 * frame, behind no ACL. The source is the per-owner RECEIVE seam rather than the graph's
 * `ctl`: this table is a transient cost of one peer's request on the receive thread, the
 * same charge the frame's own arena decode takes two calls up (ADR-0067 §3).
 *
 * Non-vacuity, as in the two cases above: the gate names ONE exact block shape — the request
 * the migrated `block_array_t` makes — and the permissive leg asserts `served()` first.
 * Revert the migration and the shape is never requested, so the instrument check reddens as
 * well as the refusal check.
 */
void test_reply_iov_on_the_seam() {
    std::printf("resolve_terminus's reply iov table (block_array_t over the injected rx):\n");
    gated_source_t rx;
    graph_t g;
    // The router's THIRD ctor parameter is the receive source; the label source stays the
    // default heap so a label allocation cannot be mistaken for this table's.
    fwd_router_t router(g, &tr::mem::heap_source(), &rx);
    counting_link_t link;
    (void)router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    const auto v = g.register_vertex(*p, role_t::STORED_VALUE);
    tr::view::rope_t val;
    val.append(make_value({std::uint8_t{0x08}, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00}));
    check(g.write(v, std::move(val)).has_value(), "the vertex holds a value to reply with");

    // A remote READ of that vertex: the frame terminates here, the resolver assembles a
    // FWD{REPLY} rope, and the egress gathers its links into the table under test.
    const auto read_once = [&link] {
        link.inject(b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"client"}), {}, {}));
    };

    rx.watch(reply_iov_bytes(kReplyLinks), kIovAlign, false);
    read_once();
    check(rx.served() == 1, "the reply iov table came from the router's INJECTED rx source");
    check(link.take() == 1, "the reply reached the requester");

    // Armed: refuse exactly that table. The reply is DROPPED and counted; nothing aborts.
    rx.watch(reply_iov_bytes(kReplyLinks), kIovAlign, true);
    const std::size_t before = router.drop_stats().reply_iov_dropped;
    read_once();
    check(rx.refused() >= 1, "the injector fired (an unreached try_alloc would be vacuous)");
    check(link.take() == 0, "an exhausted reply table DROPS the reply, never aborts");
    check(router.drop_stats().reply_iov_dropped == before + 1,
          "the drop is counted on reply_iov_dropped, the documented convention");

    // Disarm: the next request is answered again — the refusal left no residue.
    rx.watch(reply_iov_bytes(kReplyLinks), kIovAlign, false);
    read_once();
    check(link.take() == 1, "replies resume once the source recovers");

    // The ROPE terminus is the SECOND site, and it is not the same code path: the frame is
    // adopted as a lazy `tlv_view_t` rather than arena-decoded, and the reply is resolved
    // straight off the rope. Same table, same source, same refusal — driven here through the
    // rope ingress so a migration applied to only one of the two would redden.
    const std::vector<std::byte> req =
        b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"client"}), {}, {});
    const auto read_once_roped = [&link, &req] {
        tr::view::rope_t frame;
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(req.size());
        std::memcpy(seg->bytes.data(), req.data(), req.size());
        frame.append(tr::view::view_t::over(std::move(seg)));
        link.inject_rope(std::move(frame));
    };

    rx.watch(reply_iov_bytes(kReplyLinks), kIovAlign, false);
    read_once_roped();
    check(rx.served() == 1, "the ROPE terminus's reply table comes from the same rx source");
    check(link.take() == 1, "the rope-tier reply reached the requester");

    rx.watch(reply_iov_bytes(kReplyLinks), kIovAlign, true);
    const std::size_t before_rope = router.drop_stats().reply_iov_dropped;
    read_once_roped();
    check(rx.refused() >= 1, "the injector fired on the rope tier too");
    check(link.take() == 0, "an exhausted table DROPS the rope-tier reply, never aborts");
    check(router.drop_stats().reply_iov_dropped == before_rope + 1,
          "and counts it on the same reply_iov_dropped");

    // The COMPOSED-ROOT reply, which is why this table has no fixed bound. A branch read
    // folds every registered descendant's landed LKV into one reply rope, so the peer that
    // picks the root picks the link count: six children here already assemble a
    // TWENTY-link reply. A fixed-size stack array — the shape #1570 was first proposed with
    // — would have to be at least this wide, and the next child would overflow it; the only
    // honest answers are a growable table or a dropped reply, and this asserts the first.
    const auto branch = path_t::parse("/plant");
    (void)g.register_vertex(*branch, role_t::STORED_VALUE);
    for (const char* c : {"a", "b", "c", "d", "e", "f"}) {
        const auto child = path_t::parse(std::string("/plant/") + c);
        const auto cv = g.register_vertex(*child, role_t::STORED_VALUE);
        tr::view::rope_t cval;
        cval.append(make_value({std::uint8_t{0x08}, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00}));
        (void)g.write(cv, std::move(cval));
    }
    const auto read_branch = [&link] {
        link.inject(b_fwd(fwd_op_t::READ, b_path({"plant"}), b_path({"client"}), {}, {}));
    };

    // MEASURED. The exact width is incidental — what is load-bearing is that it is far past
    // any small fixed array, and that the seam is asked for exactly this one block.
    constexpr std::size_t kComposedLinks = 20;
    static_assert(kComposedLinks > 4, "the composed reply must exceed any fixed-array proposal");
    rx.watch(reply_iov_bytes(kComposedLinks), kIovAlign, false);
    read_branch();
    check(rx.served() == 1, "a 20-link composed-root reply draws ONE table of exactly that width");
    check(link.take() == 1, "the composed-root reply goes out whole");

    // And it degrades the same way: refused, not truncated and not aborted.
    rx.watch(reply_iov_bytes(kComposedLinks), kIovAlign, true);
    const std::size_t before_composed = router.drop_stats().reply_iov_dropped;
    read_branch();
    check(rx.refused() >= 1, "the injector fired on the composed reply");
    check(link.take() == 0, "an exhausted table drops the composed reply whole");
    check(router.drop_stats().reply_iov_dropped == before_composed + 1,
          "counted on reply_iov_dropped at composed-root size too");
}

// --- (d) the composed-read NODE TABLE, on a source ALLOCATOR (#873 phase 1) ---

/**
 * @brief `read_subtree_folded`'s node table draws from the graph's injected source too, and
 *        its exhaustion is BACKPRESSURE.
 *
 * Section (a) pins the collect STACK, which took `block_array_t` because its element is two
 * words. The node table beside it could not: `snap_node_t` owns a `std::shared_ptr`, so a
 * memcpy relocation would tear it, and the site carried a `#981 residual` note saying it was
 * stranded on the global heap. #873 phase 1 closed it with a source ALLOCATOR — the container
 * and element type are unchanged, only the block moves. This is the ablation for that: revert
 * the allocator and `served()` reads 0, because the growth goes back to `operator new`.
 */
void test_node_table_on_the_seam() {
    std::printf("read_subtree_folded's node table (std::vector over a source allocator):\n");
    gated_source_t src;
    graph_t g(&src);

    const auto root_path = path_t::parse("/plant");
    auto root = g.register_vertex(*root_path, role_t::STORED_VALUE);
    for (std::string_view leaf : {"/plant/t", "/plant/h", "/plant/p"}) {
        const auto p = path_t::parse(leaf);
        auto v = g.register_vertex(*p, role_t::STORED_VALUE);
        tr::view::rope_t val;
        val.append(make_value({0x08, 0x00, 0x01, 0x00, 0x2A}));
        (void)g.write(v, std::move(val));
    }

    src.watch(kNodeTableBytes, kNodeTableAlign, false);
    const auto ok = g.read_subtree_folded(root, "peer");
    check(ok.has_value(), "the composed read succeeds with a permissive source");
    check(src.served() >= 1, "the node table took its first block from the INJECTED source");

    src.watch(kNodeTableBytes, kNodeTableAlign, true);
    const auto refused = g.read_subtree_folded(root, "peer");
    check(src.refused() >= 1, "the injector fired (an unreached growth would be vacuous)");
    check(!refused.has_value() && refused.error() == tr::graph::status_t::BACKPRESSURE,
          "an exhausted node table answers BACKPRESSURE, never an abort");
}

}  // namespace

int main() {
    std::printf("#981/#1570 — migrated try_* sites answer seam exhaustion by value\n\n");
    test_collect_stack_on_the_seam();
    test_node_table_on_the_seam();
    test_delivery_iov_on_the_seam();
    test_reply_iov_on_the_seam();
    return tr::testing::summary("try_probe_window");
}
