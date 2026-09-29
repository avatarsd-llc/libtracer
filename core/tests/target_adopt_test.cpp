/**
 * @file
 * @brief Target adopt — a subscription's target leg takes a reference on the published block
 *        instead of minting one of its own (RFC-0028 slice 4, D2; the fan-out half of #1620).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What slice 4 promises, pinned against the graph's INJECTED source so the count is exact:
 *
 *   - K stored targets cost ONE value block per publish, not K + 1: every target reads back
 *     the very block the source published (identity, not equal bytes);
 *   - the replaced block is still returned — once every slot that adopted it has moved on;
 *   - the target's admission filter still runs on the adopting arm: admitted unchanged it
 *     costs nothing, and a normalisation is the only thing that mints the target a block;
 *   - a STREAM target's ring admits the adopted block;
 *   - the two shapes that CANNOT be adopted still land by a clone: a value in caller-owned
 *     storage (a HANDLER source's unstored delivery) and a HANDLER target.
 *
 * On `main` before slice 4 the first vector fails (33 blocks for 32 targets, and no target
 * shares the source's block), as does the admission vector's identity check.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

#include "libtracer/graph.hpp"
#include "libtracer/value.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::admission_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::value_t;
using tr::graph::write_ctx_t;
using tr::testing::check;
using tr::testing::make_value;
using tr::view::rope_t;

/** @brief Whether the bound slot drops a displaced value's reference AT the swap (see
 *         value_test: `hazard_slot_t` returns it at a later grace period instead). */
constexpr bool kSlotReleasesAtSwap =
    std::is_same_v<tr::graph::config_t::lkv_slot_t, tr::graph::single_writer_slot_t>;

/**
 * @brief A source that counts draws and releases of the one-link value-block shape.
 *
 * Every other shape the graph draws from it is served uncounted, so the counts read as
 * "value blocks" without the test knowing the graph's other channels.
 */
class value_meter_t final : public tr::mem::block_source_t {
   public:
    value_meter_t() noexcept : block_source_t("adopt-meter") {}

    /** @brief Zero the counters (the watched shape is the one-link value block). */
    void reset() noexcept {
        served_ = 0;
        released_ = 0;
    }
    /** @brief Value blocks drawn since @ref reset. */
    [[nodiscard]] std::size_t served() const noexcept { return served_; }
    /** @brief Value blocks returned since @ref reset. */
    [[nodiscard]] std::size_t released() const noexcept { return released_; }

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (watched(bytes, align)) ++served_;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        if (watched(bytes, align)) ++released_;
        ::operator delete(p, std::align_val_t{align});
    }

   private:
    /** @brief The one-link value block's shape. */
    [[nodiscard]] static bool watched(std::size_t bytes, std::size_t align) noexcept {
        return bytes == value_t::bytes_for(1) && align == value_t::kAlign;
    }

    std::size_t served_ = 0;
    std::size_t released_ = 0;
};

/** @brief A one-link, one-byte value. */
[[nodiscard]] rope_t byte_value(std::uint8_t b) { return rope_t{make_value({b})}; }

/** @brief The byte a one-link, one-byte value carries (0 when it is not that shape). */
[[nodiscard]] std::uint8_t only_byte(const value_t& v) {
    if (v.link_count() != 1 || v.total_length() != 1) return 0;
    return std::to_integer<std::uint8_t>(v.only().bytes()[0]);
}

/** @brief The same, on a rope an admission filter or a handler is handed. */
[[nodiscard]] std::uint8_t only_byte(const rope_t& r) {
    return only_byte(tr::graph::value_storage_t<2>{r}.get());
}

/** @brief The block @p p currently holds, or null. */
[[nodiscard]] const value_t* block_at(graph_t& g, const path_t& p) {
    const auto r = g.read(p);
    return r ? r->get() : nullptr;
}

/** @brief Vector 1 — thirty-two stored targets, ONE block per publish, all of them shared. */
void test_fan_out_is_one_block() {
    std::printf("vector 1 — K=32 targets adopt the one published block:\n");
    constexpr int kTargets = 32;
    value_meter_t src;
    graph_t g(&src);
    const auto s = g.register_vertex(path_t("/a/src"), role_t::STORED_VALUE);
    std::array<path_t, kTargets> targets;
    for (int i = 0; i < kTargets; ++i) {
        targets[i] = path_t("/a/t" + std::to_string(i));
        (void)g.register_vertex(targets[i], role_t::STORED_VALUE);
        check(g.subscribe(path_t("/a/src"), targets[i]).has_value(), "wire src -> target");
    }

    src.reset();
    check(g.write(s, byte_value(0x41)).has_value(), "the write lands");
    check(src.served() == 1, "ONE value block for the publish and all 32 target deliveries");
    const value_t* published = block_at(g, path_t("/a/src"));
    bool all_shared = published != nullptr;
    for (const path_t& t : targets) all_shared = all_shared && block_at(g, t) == published;
    check(all_shared, "every target holds the SAME block the source published");
    check(published != nullptr && only_byte(*published) == 0x41, "carrying the written byte");
    const auto d = g.delivery_drops();
    check(d.no_target + d.denied + d.out_of_memory == 0, "and no delivery was dropped");

    src.reset();
    check(g.write(s, byte_value(0x42)).has_value(), "a second write lands");
    check(src.served() == 1, "again one block");
    if constexpr (kSlotReleasesAtSwap) {
        check(src.released() == 1,
              "and the first block went back once the source and all 32 targets moved off it");
    }
    for (const path_t& t : targets) {
        const value_t* now = block_at(g, t);
        if (now == nullptr || only_byte(*now) != 0x42) {
            check(false, "every target reads the second value");
            return;
        }
    }
    check(true, "every target reads the second value");
}

/** @brief Vector 2 — the admission filter runs on the adopting arm; only a normalisation
 *         mints the target a block. */
void test_admission_on_the_shared_block() {
    std::printf("vector 2 — the target's admission filter sees the shared block:\n");
    value_meter_t src;
    graph_t g(&src);
    int seen = 0;
    handlers_t h;
    h.on_admit = [&seen](const rope_t& value, const write_ctx_t&) -> admission_t {
        seen = only_byte(value);
        if (seen == 0x10) return rope_t{make_value({0x11})};  // normalise 0x10 -> 0x11
        return std::nullopt;
    };
    const auto s = g.register_vertex(path_t("/b/src"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/b/sink"), role_t::STORED_VALUE, std::move(h));
    check(g.subscribe(path_t("/b/src"), path_t("/b/sink")).has_value(), "wire src -> sink");

    src.reset();
    check(g.write(s, byte_value(0x20)).has_value(), "an admitted value flows through");
    check(seen == 0x20, "the filter saw the delivered bytes");
    check(block_at(g, path_t("/b/sink")) == block_at(g, path_t("/b/src")),
          "admitted unchanged, the target ADOPTS the source's block");
    check(src.served() == 1, "and the delivery drew nothing");

    src.reset();
    check(g.write(s, byte_value(0x10)).has_value(), "a normalised value flows through");
    const value_t* sink = block_at(g, path_t("/b/sink"));
    check(sink != nullptr && sink != block_at(g, path_t("/b/src")) && only_byte(*sink) == 0x11,
          "the normalisation is the target's OWN block, holding the filter's bytes");
    check(src.served() == 2, "the one extra block is the normalisation's");
}

/** @brief Vector 3 — a STREAM target's ring admits the adopted value. */
void test_stream_target() {
    std::printf("vector 3 — a STREAM target's ring holds the adopted value:\n");
    graph_t g;
    const auto s = g.register_vertex(path_t("/c/src"), role_t::STORED_VALUE);
    const auto t = g.register_vertex(path_t("/c/log"), role_t::STREAM);
    g.set_history_depth(t, 4);
    check(g.subscribe(path_t("/c/src"), path_t("/c/log")).has_value(), "wire src -> log");
    check(g.write(s, byte_value(0x31)).has_value() && g.write(s, byte_value(0x32)).has_value(),
          "two writes land");
    const auto hist = g.history(t);
    check(hist.has_value() && hist->size() == 2, "the ring admitted both deliveries");
    check(hist.has_value() && hist->size() == 2 && only_byte((*hist)[0]) == 0x31 &&
              only_byte((*hist)[1]) == 0x32,
          "in order, with the source's bytes");
    check(block_at(g, path_t("/c/log")) == block_at(g, path_t("/c/src")),
          "and the target's last value is the source's block");
}

/** @brief Vector 4 — a value in caller-owned storage cannot be adopted, and lands by a clone. */
void test_unstored_source_is_cloned() {
    std::printf("vector 4 — a HANDLER source's unstored delivery is cloned, not adopted:\n");
    value_meter_t src;
    graph_t g(&src);
    handlers_t h;
    h.on_write = [](const rope_t&, const write_ctx_t&) -> tr::graph::result_t<void> { return {}; };
    const auto s = g.register_vertex(path_t("/d/act"), role_t::HANDLER, std::move(h));
    (void)g.register_vertex(path_t("/d/mirror"), role_t::STORED_VALUE);
    check(g.subscribe(path_t("/d/act"), path_t("/d/mirror")).has_value(), "wire act -> mirror");
    src.reset();
    check(g.write(s, byte_value(0x51)).has_value(), "the handler write lands");
    const value_t* mirror = block_at(g, path_t("/d/mirror"));
    check(mirror != nullptr && only_byte(*mirror) == 0x51,
          "the target holds the value the handler consumed");
    check(mirror != nullptr && mirror->source() == &src,
          "in a block of its OWN, from the graph's source — not the caller's stack storage");
    check(src.served() == 1, "one block: the target's clone");
}

/** @brief Vector 5 — a HANDLER target reads a rope, so its leg clones rather than adopts. */
void test_handler_target() {
    std::printf("vector 5 — a HANDLER target is handed the delivered bytes:\n");
    graph_t g;
    int seen = 0;
    handlers_t h;
    h.on_write = [&seen](const rope_t& value, const write_ctx_t&) -> tr::graph::result_t<void> {
        seen = only_byte(value);
        return {};
    };
    const auto s = g.register_vertex(path_t("/e/src"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/e/act"), role_t::HANDLER, std::move(h));
    check(g.subscribe(path_t("/e/src"), path_t("/e/act")).has_value(), "wire src -> act");
    check(g.write(s, byte_value(0x61)).has_value(), "the write lands");
    check(seen == 0x61, "the handler target saw the source's bytes");
    check(g.delivery_drops().out_of_memory == 0, "with no drop");
}

}  // namespace

int main() {
    test_fan_out_is_one_block();
    test_admission_on_the_shared_block();
    test_stream_target();
    test_unstored_source_is_cloned();
    test_handler_target();
    return tr::testing::summary("target_adopt");
}
