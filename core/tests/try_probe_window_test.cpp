/**
 * @file
 * @brief #981 — the two `try_*` call sites that MIGRATED off the `-fno-exceptions` probe
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
 * Two sites migrated, and this file is their proof:
 *
 *   - `graph_t::read_subtree_folded`'s collect stack (`work_t` = a vertex pointer plus an
 *     index) — a peer picks which composed root to READ, so it picks how far this grows;
 *   - `fwd_router_t::deliver_remote`'s egress iov tables (`std::span`), both the
 *     reverse-list arm and the canonical full-route arm — one per remote delivery.
 *
 * @section instrument Why neither check is vacuous
 *
 * A "soft-fails on exhaustion" assertion passes trivially if the code under test never
 * consults the source it is being starved of — exactly what the PRE-migration code does,
 * since it allocated from the GLOBAL heap and the injected `ctl` source saw nothing. So the
 * gate here is not "refuse everything": @ref gated_source_t refuses ONE exact block shape,
 * the `(elements * sizeof(T), alignof(T))` request the migrated container makes, and every
 * armed case asserts a POSITIVE instrument first — `served()` shows the source was really
 * asked for that shape. Revert either migration and the shape is never requested: the
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
#include <new>
#include <span>
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
    graph_t g(std::pmr::get_default_resource(), &tr::mem::heap_backend(), &src);

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
    graph_t g(std::pmr::get_default_resource(), &tr::mem::heap_backend(), &src);
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

}  // namespace

int main() {
    std::printf("#981 — migrated try_* sites answer seam exhaustion by value\n\n");
    test_collect_stack_on_the_seam();
    test_delivery_iov_on_the_seam();
    return tr::testing::summary("try_probe_window");
}
