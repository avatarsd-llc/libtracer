/**
 * @file
 * @brief `value_t` — the one block a publish costs (RFC-0028 slice 3, D1 + the first half of D9).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What slice 3 promises, pinned where it can be measured:
 *
 *   - the block shape: a header of two words plus one `view_t` per link, so a one-link value
 *     is 40 B on a 64-bit host (the RFC §6.3 gate is <= 40 B) and `sizeof(vertex_t)` is 88
 *     there — one word for the slot where `std::shared_ptr` took two;
 *   - one `try_alloc` per publish, from the vertex's INJECTED source and of exactly that
 *     shape, released back to the same source by the publish that replaces it;
 *   - the handle: `adopt` takes the reference, `share` and a copy add one, `reset` and the
 *     destructor drop one, and the last drop hands the block back — the reader that parks a
 *     `value_ref_t` is the one thing that keeps a replaced value's block alive;
 *   - a HANDLER sink sees the written links through `const value_t&` without a draw
 *     (`value_storage_t` on the caller's stack);
 *   - exhaustion is by value: a source that refuses the block turns the write into
 *     `BACKPRESSURE` and the prior value survives untouched.
 *
 * On `main` before slice 3 this file does not compile (`libtracer/value.hpp` did not exist), and
 * its shape checks would fail if it did (104 B per publish, 96 B per vertex).
 */

#include "libtracer/value.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <span>
#include <type_traits>

#include "libtracer/graph.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::value_ref_t;
using tr::graph::value_storage_t;
using tr::graph::value_t;
using tr::graph::vertex_t;
using tr::testing::check;
using tr::testing::make_value;
using tr::view::rope_t;
using tr::view::view_t;

/** @brief The size a one-link value must fit in on the host (RFC-0028 §6.3's slice-3 gate). */
constexpr std::size_t kOneLinkGateBytes = 40;

/**
 * @brief Whether the bound slot drops a replaced value's reference AT the swap.
 *
 * `single_writer_slot_t` releases the displaced value as `store` returns, so a replaced block
 * goes back to its source the moment the last handle drops. `hazard_slot_t` parks the displaced
 * NODE — which owns that reference — on a retire list that a later scan drains once no reader
 * announces it (ADR-0069), so the same block returns at a grace period, not synchronously; the
 * "returns to the source now" checks below are true of the first policy only and are asserted
 * on that binding, while the block's INTEGRITY is asserted on both.
 */
constexpr bool kSlotReleasesAtSwap =
    std::is_same_v<tr::graph::config_t::lkv_slot_t, tr::graph::single_writer_slot_t>;

/**
 * @brief A source that counts draws of the value-block shape, remembers how many of its blocks
 *        are live, and can be told to refuse the next value block.
 *
 * Everything else the graph draws from it (control-plane containers, registries) is served
 * from the heap uncounted, so the counts read as "value blocks" without the test having to
 * know the graph's other channels.
 */
class value_meter_t final : public tr::mem::block_source_t {
   public:
    value_meter_t() noexcept : block_source_t("value-meter") {}

    /** @brief Arm for values carrying @p links links. */
    void watch(std::size_t links) noexcept {
        bytes_ = value_t::bytes_for(links);
        served_ = 0;
        released_ = 0;
        other_ = 0;
    }
    /** @brief Refuse the value shape from now on (until the next @ref watch). */
    void refuse(bool on) noexcept { refuse_ = on; }

    [[nodiscard]] std::size_t served() const noexcept { return served_; }
    [[nodiscard]] std::size_t released() const noexcept { return released_; }
    /** @brief Draws that were NOT the watched shape while armed — should stay 0 on a write. */
    [[nodiscard]] std::size_t other() const noexcept { return other_; }
    [[nodiscard]] std::size_t live() const noexcept { return served_ - released_; }

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (bytes == bytes_ && align == value_t::kAlign) {
            if (refuse_) return nullptr;
            ++served_;
        } else if (bytes_ != 0) {
            ++other_;
        }
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        if (bytes == bytes_ && align == value_t::kAlign) ++released_;
        ::operator delete(p, std::align_val_t{align});
    }

   private:
    std::size_t bytes_ = 0;
    bool refuse_ = false;
    std::size_t served_ = 0;
    std::size_t released_ = 0;
    std::size_t other_ = 0;
};

/** @brief The byte a one-byte value carries (0 when it is not that shape). */
[[nodiscard]] std::uint8_t only_byte(const value_t& v) {
    if (v.link_count() != 1 || v.total_length() != 1) return 0;
    return std::to_integer<std::uint8_t>(v.only().bytes()[0]);
}

/** @brief The shapes the slice pins, on the host. */
void test_shapes() {
    std::printf("shapes:\n");
    check(value_t::bytes_for(0) == sizeof(value_t), "an empty value is just its header");
    check(value_t::bytes_for(1) == sizeof(value_t) + sizeof(view_t),
          "one link adds exactly one view_t");
    if constexpr (sizeof(void*) == 8) {
        check(sizeof(value_t) == 16, "the header is two words on a 64-bit host");
        check(value_t::bytes_for(1) <= kOneLinkGateBytes,
              "a one-link publish fits RFC-0028 §6.3's <= 40 B gate");
        check(sizeof(vertex_t) <= 88, "the vertex lost the shared_ptr's second word (96 -> 88)");
        check(sizeof(value_ref_t) == sizeof(void*), "the handle is one word");
    }
    std::printf("    sizeof(value_t)=%zu bytes_for(1)=%zu sizeof(vertex_t)=%zu\n", sizeof(value_t),
                value_t::bytes_for(1), sizeof(vertex_t));
}

/** @brief The handle's reference discipline over a hand-minted value. */
void test_handle() {
    std::printf("handle:\n");
    value_meter_t src;
    src.watch(1);
    const view_t link = make_value({0x5A});
    value_t* raw = value_t::make(std::span<const view_t>(&link, 1), src);
    check(raw != nullptr && src.served() == 1, "make draws exactly one block of the value shape");
    check(raw->use_count() == 1 && raw->source() == &src, "the maker holds the one reference");
    check(raw->block_bytes() == value_t::bytes_for(1), "and the block knows its own size");
    {
        const value_ref_t owner = value_ref_t::adopt(raw);
        check(owner && owner.get() == raw && owner->use_count() == 1,
              "adopt takes the reference without adding one");
        const value_ref_t shared = value_ref_t::share(raw);
        value_ref_t copy = owner;
        check(raw->use_count() == 3, "share and a copy each add one");
        value_ref_t moved = std::move(copy);
        check(raw->use_count() == 3 && moved.get() == raw, "a move adds none");
        moved.reset();
        check(raw->use_count() == 2 && !moved, "reset drops one and empties the handle");
        check(only_byte(*owner) == 0x5A, "the links read back through the handle");
        check(src.live() == 1, "the block is live while any handle holds it");
    }
    check(src.live() == 0 && src.released() == 1,
          "the last handle's destructor hands the block back to its source, sized");

    rope_t r;
    r.append(make_value({0x01}));
    r.append(make_value({0x02}));
    src.watch(2);
    const value_ref_t two = value_ref_t::adopt(value_t::make(std::move(r), src));
    check(two && two->link_count() == 2 && two->total_length() == 2 && src.served() == 1,
          "the rope form moves the links into one two-link block");
    check(r.links().empty(), "and leaves the rope empty");

    const value_storage_t<1> on_stack{link};
    check(on_stack.get().use_count() == 1 && on_stack.get().source() == nullptr,
          "value_storage_t is a value with no source: nothing to give back");
    check(only_byte(on_stack.get()) == 0x5A, "and it carries the link it was built over");
}

/** @brief One block per publish from the injected source, replaced block returned. */
void test_one_block_per_publish() {
    std::printf("one block per publish:\n");
    value_meter_t src;
    graph_t g(src);
    const auto v = g.register_vertex(path_t("/v/a"), role_t::STORED_VALUE);

    src.watch(1);
    rope_t first;
    first.append(make_value({0x11}));
    check(g.write(v, std::move(first)).has_value(), "the first write lands");
    check(src.served() == 1, "and drew exactly one value block from the INJECTED source");
    check(src.other() == 0, "and nothing else of any other shape");

    value_ref_t parked;
    {
        const auto r = g.read(v);
        check(r.has_value() && only_byte(**r) == 0x11, "read hands back the stored value");
        check((*r)->use_count() == 2, "held by the slot and by the reader");
        check((*r)->source() == &src, "the value knows the source it came from");
        parked = *r;
    }

    rope_t second;
    second.append(make_value({0x22}));
    check(g.write(v, std::move(second)).has_value(), "the replacing write lands");
    check(src.served() == 2 && src.released() == 0,
          "one more block; the replaced one is still alive — a reader parked it");
    check(only_byte(*parked) == 0x11, "the parked handle reads the OLD bytes intact");
    if constexpr (kSlotReleasesAtSwap) {
        check(parked->use_count() == 1, "and is now the replaced value's only owner");
        parked.reset();
        check(src.released() == 1 && src.live() == 1,
              "dropping the parked handle returns the replaced block to the source");
    } else {
        // The hazard slot's retired node still owns one reference until a scan drains it.
        check(parked->use_count() >= 1, "and the retired node may still share it (hazard)");
        parked.reset();
        check(src.live() >= 1, "the current value's block is live either way");
    }

    rope_t third;
    third.append(make_value({0x33}));
    check(g.write(v, std::move(third)).has_value(), "a third write lands");
    check(src.served() == 3, "which drew its own one block");
    if constexpr (kSlotReleasesAtSwap) {
        check(src.released() == 2, "with no reader parked, the replaced block goes straight back");
    } else {
        check(src.released() <= 2, "the hazard slot returns replaced blocks at its next scan");
    }
}

/** @brief A HANDLER sink sees the written links as a `value_t` with no block drawn. */
void test_handler_sees_value_without_a_draw() {
    std::printf("handler delivery:\n");
    value_meter_t src;
    graph_t g(src);
    const auto producer = g.register_vertex(path_t("/h/src"), role_t::STORED_VALUE);
    struct seen_t {
        int calls = 0;
        std::uint8_t byte = 0;
        std::uint32_t refs = 0;
    } seen;
    auto sink = [&seen](const value_t& v) {
        ++seen.calls;
        seen.byte = only_byte(v);
        seen.refs = v.use_count();
    };
    const auto sub = g.subscribe(path_t("/h/src"), sink);
    check(sub.has_value(), "the subscription is taken");

    src.watch(1);
    rope_t value;
    value.append(make_value({0x77}));
    check(g.write(producer, std::move(value)).has_value(), "the write lands");
    check(seen.calls == 1 && seen.byte == 0x77, "the sink saw the value's one link");
    check(seen.refs >= 1, "the value is owned for the whole callback");
    check(src.served() == 1, "a STORED_VALUE producer draws its one block, and no other");
}

/** @brief Exhaustion is by value: a refused block is BACKPRESSURE, the prior value intact. */
void test_refusal_is_backpressure() {
    std::printf("refusal:\n");
    value_meter_t src;
    graph_t g(src);
    const auto v = g.register_vertex(path_t("/r/a"), role_t::STORED_VALUE);
    src.watch(1);
    rope_t first;
    first.append(make_value({0x44}));
    check(g.write(v, std::move(first)).has_value(), "the first write lands");

    src.refuse(true);
    rope_t second;
    second.append(make_value({0x55}));
    const auto w = g.write(v, std::move(second));
    check(!w.has_value() && w.error() == status_t::BACKPRESSURE,
          "a refused value block answers BACKPRESSURE, never an abort");
    const auto r = g.read(v);
    check(r.has_value() && only_byte(**r) == 0x44, "the prior value survives untouched");
    check(src.live() == 1, "and no block leaked on the refused path");

    src.refuse(false);
    rope_t third;
    third.append(make_value({0x66}));
    check(g.write(v, std::move(third)).has_value(), "the vertex recovers once the source does");
}

}  // namespace

/**
 * @brief A source that counts every draw and release by size — for the inline arm, whose block
 *        size depends on the payload, not the link count.
 */
class block_meter_t final : public tr::mem::block_source_t {
   public:
    block_meter_t() noexcept : block_source_t("block-meter") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        ++draws;
        last_bytes = bytes;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ++releases;
        released_bytes = bytes;
        ::operator delete(p, std::align_val_t{align});
    }
    std::size_t draws = 0;          /**< @brief Blocks served. */
    std::size_t releases = 0;       /**< @brief Blocks handed back. */
    std::size_t last_bytes = 0;     /**< @brief Size of the last draw. */
    std::size_t released_bytes = 0; /**< @brief Size of the last release (sized reclaim). */
};

/**
 * @brief RFC-0028 §5.1's inline arm (slice 5): a copied value is ONE block of header + link +
 *        embedded segment + bytes; a rope a sink cloned out of it keeps the block alive past
 *        the value; a rope that is exactly a live inline value's bytes is adopted by `make`.
 */
void test_inline_arm() {
    std::printf("inline arm:\n");
    const std::array<std::byte, 20> bytes{std::byte{0xC3}, std::byte{0x01}, std::byte{0x02}};
    if constexpr (sizeof(void*) == 8) {
        check(value_t::inline_bytes_for(0) == 80,
              "the inline overhead on the host is 80 B: 16 header + 24 link + 40 segment");
    } else {
        check(value_t::inline_bytes_for(0) == 44,
              "the inline overhead on a 32-bit target is 44 B: 12 header + 12 link + 20 segment");
    }
    std::printf("    inline_bytes_for(0)=%zu  sizeof(segment_t)=%zu\n",
                value_t::inline_bytes_for(0), sizeof(tr::view::segment_t));

    block_meter_t src;
    value_t* v = value_t::make_copy(bytes, src);
    check(v != nullptr && src.draws == 1 && src.last_bytes == value_t::inline_bytes_for(20),
          "make_copy draws exactly ONE block of inline_bytes_for(len)");
    check(v->is_inline() && v->link_count() == 1 && v->total_length() == 20 &&
              v->block_bytes() == value_t::inline_bytes_for(20),
          "the value is inline, one link long, and knows its block size");
    check(std::memcmp(v->only().bytes().data(), bytes.data(), bytes.size()) == 0,
          "the link reads the copied bytes");
    check(!v->only().is_device() && v->all_host(), "the embedded segment is host memory");

    // make(rope over the value's whole bytes) ADOPTS it: same block, one more reference.
    {
        rope_t r;
        r.append(v->only());
        value_t* same = value_t::make(std::move(r), src);
        check(same == v && src.draws == 1 && v->use_count() == 2 && r.link_count() == 0,
              "a rope over a live inline value's bytes is adopted, not re-drawn");
        value_t::release(same);
    }
    // A SUBVIEW is a different value: it publishes as a link block over this one's segment.
    {
        rope_t r;
        r.append(v->only().subview(1, 4));
        value_t* sub = value_t::make(std::move(r), src);
        check(sub != nullptr && sub != v && !sub->is_inline() && src.draws == 2,
              "a subview rope draws its own link block");
        value_t::release(sub);
    }

    // A rope a sink cloned out keeps the BLOCK alive past the last value reference.
    rope_t kept = v->rope();
    const std::size_t releases_before = src.releases;
    value_t::release(v);  // the last VALUE reference
    check(src.releases == releases_before,
          "the value is gone but the block is not — the kept rope holds its segment");
    check(kept.total_length() == 20 &&
              std::memcmp(kept.links()[0].bytes().data(), bytes.data(), bytes.size()) == 0,
          "and the kept rope still reads the bytes");
    // ...and a rope over a DEAD inline value is not resurrected: `make` draws a link block.
    {
        rope_t r = kept;
        value_t* fresh = value_t::make(std::move(r), src);
        check(fresh != nullptr && fresh != v && !fresh->is_inline(),
              "a rope over a dead inline value publishes a new link block (no resurrection)");
        value_t::release(fresh);
    }
    kept = rope_t{};
    check(
        src.releases == releases_before + 2 && src.released_bytes == value_t::inline_bytes_for(20),
        "the last segment reference returns the WHOLE inline block, sized");

    // Exhaustion is nullptr by value.
    tr::mem::null_source_t none;
    check(value_t::make_inline(8, none) == nullptr, "a refused inline block is nullptr");
}

int main() {
    std::printf("value_t — one block per publish (RFC-0028 slice 3)\n\n");
    test_shapes();
    test_handle();
    test_one_block_per_publish();
    test_handler_sees_value_without_a_draw();
    test_refusal_is_backpressure();
    test_inline_arm();
    return tr::testing::summary("value");
}
