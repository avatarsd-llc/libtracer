/**
 * @file
 * @brief RFC-0030 §5 — the path object: made from a string or a borrowed ROM payload, resolved
 *        through its element 0, falling back to the canonical bytes when that goes stale, and
 *        carrying a soft, inline, compile-time-sized chain cache.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every door case is checked against the canonical form of the same operation: the path object
 * is only a faster way to the answer the string gives, never a different answer.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace tr::graph {
/** @brief The test-only door: writes a path object's element 0 directly. */
struct path_object_test_door_t {
    static void set(const path_object_t& p, std::optional<vertex_slot_t> e) noexcept {
        p.set_element0(e);
    }
};
}  // namespace tr::graph

namespace {

using tr::graph::graph_t;
using tr::graph::kPathChainCapacity;
using tr::graph::path_object_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief The first byte of @p g's value at @p p, or -1 when the read is refused. */
int first_byte(const graph_t& g, const path_object_t& p) {
    const auto r = g.read(p);
    if (!r || (*r)->only().bytes().empty()) return -1;
    return static_cast<int>((*r)->only().bytes()[0]);
}

/** @brief The same through the canonical string form. */
int first_byte(const graph_t& g, const path_t& p) {
    const auto r = g.read(p);
    if (!r || (*r)->only().bytes().empty()) return -1;
    return static_cast<int>((*r)->only().bytes()[0]);
}

/** @brief A string makes an object over its canonical bytes; repeated doors use the pair. */
void test_from_string() {
    std::printf("made from a string, resolved through element 0:\n");
    graph_t g;
    const path_t canon = *path_t::parse("/sensor/temp");
    const vertex_handle_t v = g.register_vertex(canon, role_t::STORED_VALUE);
    check(g.write(v, make_value({7})).has_value(), "seeded the vertex");

    auto made = g.make_path("/sensor/temp");
    check(made.has_value(), "make_path accepts a canonical string");
    const path_object_t& p = *made;
    check(std::ranges::equal(p.key(), canon.key()), "the object holds the canonical bytes");
    check(!p.element0().has_value(), "nothing is resolved before the first door");

    check(first_byte(g, p) == 7 && first_byte(g, canon) == 7, "a read answers as the string");
    const auto e = p.element0();
    check(e.has_value() && e == g.vertex_slot(v), "the first door learned the vertex's pair");
    check(g.write(p, make_value({8})).has_value() && first_byte(g, canon) == 8,
          "a write through the object lands on the vertex");
    check(first_byte(g, p) == 8 && p.element0() == e, "a repeated door keeps the same pair");
    const auto d = g.deref_vertex_slot(e->index, e->generation);
    check(d.has_value() && *d == v, "... and that pair dereferences to the vertex");

    int fired = 0;
    const auto sub =
        g.subscribe(p, [](void* c, const tr::graph::value_t&) { ++*static_cast<int*>(c); }, &fired);
    check(sub.has_value(), "subscribe takes the object");
    check(g.write(p, make_value({9})).has_value() && fired == 1,
          "a write through the object reaches the subscriber");
    (void)g.unsubscribe(*sub);

    tr::graph::field_path_t settings;
    (void)settings.steps.push_back({.name = "settings"});
    const auto fr = g.read(p, settings);
    const auto fc = g.read(v, settings);
    check(fr.has_value() == fc.has_value(), "a field read answers as the handle's");
}

/** @brief A retired or re-registered target falls back to the canonical bytes. */
void test_stale_falls_back() {
    std::printf("a stale element 0 falls back to the canonical bytes:\n");
    graph_t g;
    const path_t canon = *path_t::parse("/motor/speed");
    const vertex_handle_t v = g.register_vertex(canon, role_t::STORED_VALUE);
    (void)g.write(v, make_value({1}));
    auto p = *g.make_path("/motor/speed");
    check(first_byte(g, p) == 1, "the first read resolves");
    const auto old = p.element0();

    // Retired behind the object's back, then a successor registered at the same name.
    check(g.retire(v).has_value(), "retired the vertex by its handle");
    check(!g.read(p).has_value() && !g.read(canon).has_value(),
          "with nothing registered both forms answer NOT_FOUND");
    check(!p.element0().has_value(), "... and the miss cleared element 0");
    const vertex_handle_t v2 = g.register_vertex(canon, role_t::STORED_VALUE);
    (void)g.write(v2, make_value({2}));
    check(first_byte(g, p) == 2 && first_byte(g, canon) == 2,
          "the successor answers through the object as through the string");
    const auto now = p.element0();
    check(now.has_value() && now == g.vertex_slot(v2), "the object re-learned the successor");
    check(old.has_value() && now != old, "... under a new pair");

    // Retired while the object still holds a pair the slot no longer honours.
    const auto held = p.element0();
    check(g.retire(v2).has_value(), "retired the successor");
    const vertex_handle_t v3 = g.register_vertex(canon, role_t::STORED_VALUE);
    (void)g.write(v3, make_value({3}));
    check(p.element0() == held, "the object still holds the stale pair");
    check(first_byte(g, p) == 3, "the stale pair is refused and the canonical bytes answer");

    check(g.retire(p).has_value(), "retire takes the object");
    check(!p.element0().has_value(), "... and drops its element 0");
    check(!g.read(p).has_value(), "after the retire the object reads NOT_FOUND");
    const auto miss = g.write(p, make_value({4}));
    check(!miss.has_value() && miss.error() == status_t::NOT_FOUND,
          "a write to a missing target creates nothing without a creation hook");
}

/** @brief Pre-encoded `/rom/x`: the packed records `03 'r' 'o' 'm' 01 'x'`. */
constexpr std::array<std::byte, 6> kRomKey = {std::byte{3},   std::byte{'r'}, std::byte{'o'},
                                              std::byte{'m'}, std::byte{1},   std::byte{'x'}};

/** @brief A ROM payload is borrowed; malformed inputs are refused by value. */
void test_rom_and_refusals() {
    std::printf("a ROM payload is borrowed, and bad input refused:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(*path_t::parse("/rom/x"), role_t::STORED_VALUE);
    (void)g.write(v, make_value({5}));
    auto rom = g.make_path(std::span<const std::byte>(kRomKey));
    check(rom.has_value() && rom->key().data() == kRomKey.data(),
          "the object borrows the ROM bytes rather than copying them");
    check(rom.has_value() && first_byte(g, *rom) == 5, "... and resolves through them");

    constexpr std::array<std::byte, 3> kEscape = {std::byte{0}, std::byte{0x16}, std::byte{0}};
    check(g.make_path(std::span<const std::byte>(kEscape)).error() == status_t::INVALID_PATH,
          "an escape record is not a canonical payload");
    check(g.make_path("no-root").error() == status_t::INVALID_PATH, "an unrooted string");
    check(g.make_path("/a:settings").error() == status_t::INVALID_PATH,
          "a field tail: the field door takes the field beside the object");

    const path_object_t none;
    check(none.empty() && !g.read(none).has_value() && !g.write(none, make_value({1})) &&
              !g.retire(none),
          "an empty object names nothing at every door");
}

/** @brief The chain cache is inline, bounded at compile time, and soft. */
void test_chain_cache() {
    std::printf("the chain cache (%zu bytes):\n", kPathChainCapacity);
    graph_t g;
    auto p = *g.make_path("/far/away");
    std::array<std::byte, kPathChainCapacity + 8> out{};
    check(p.chain(out) == 0, "no chain before a learn");

    std::vector<std::byte> a(11);
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<std::byte>(i + 1);
    check(p.learn(a), "a chain within capacity is cached");
    check(p.chain(out) == a.size() && std::ranges::equal(std::span(out).first(a.size()), a),
          "... and reads back byte for byte");
    std::array<std::byte, 4> small{};
    check(p.chain(small) == 0, "an output too small reads no chain");

    const std::vector<std::byte> big(kPathChainCapacity + 1, std::byte{0xAB});
    check(!p.learn(big), "a chain larger than the capacity is not cached");
    check(p.chain(out) == 0, "... and drops the chain it supersedes");

    check(p.learn(a), "re-learned");
    p.clear_chain();
    check(p.chain(out) == 0, "clear_chain drops it");
    check(!p.learn({}), "an empty chain is refused");
}

/** @brief A reader racing learns sees a whole chain or none, never a torn one. */
void test_chain_race() {
    std::printf("a learn racing readers never tears:\n");
    graph_t g;
    auto p = *g.make_path("/race");
    const std::vector<std::byte> a(9, std::byte{0x11});
    const std::vector<std::byte> b(kPathChainCapacity, std::byte{0x22});
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&] {
            std::array<std::byte, kPathChainCapacity> out{};
            while (!stop.load(std::memory_order_relaxed)) {
                const std::size_t n = p.chain(out);
                const auto got = std::span(out).first(n);
                if (n != 0 && !std::ranges::equal(got, a) && !std::ranges::equal(got, b))
                    torn.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int i = 0; i < 20000; ++i) (void)p.learn((i & 1) != 0 ? a : b);
    stop.store(true);
    for (auto& t : readers) t.join();
    check(torn.load() == 0, "every chain read was one of the learned chains, or none");
}

/** @brief A reader racing element-0 writes sees one written pair or none, never a torn one. */
void test_element0_race() {
    std::printf("an element-0 write racing readers never tears:\n");
    graph_t g;
    auto p = *g.make_path("/race0");
    // The two halves differ in both words, so a pair mixing them is visible.
    const tr::graph::vertex_slot_t a{.index = 1, .generation = 2};
    const tr::graph::vertex_slot_t b{.index = 0x10000, .generation = 0x20000};
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto e = p.element0();
                if (e && *e != a && *e != b) torn.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int i = 0; i < 20000; ++i) {
        tr::graph::path_object_test_door_t::set(p, (i & 1) != 0 ? a : b);
        if (i % 7 == 0) (void)p.learn(std::vector<std::byte>(9, std::byte{0x33}));
    }
    stop.store(true);
    for (auto& t : readers) t.join();
    check(torn.load() == 0, "every element-0 read was one of the written pairs, or none");
}

/** @brief Serves until armed, then `allow` more blocks; counts live blocks. */
class gate_source_t final : public tr::mem::block_source_t {
   public:
    gate_source_t() noexcept : tr::mem::block_source_t("gate") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_ && allow_ == 0) return nullptr;
        if (armed_) --allow_;
        void* const q = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
        if (q != nullptr) ++live_;
        return q;
    }
    void release(void* q, std::size_t bytes, std::size_t align) noexcept override {
        --live_;
        ::operator delete(q, bytes, std::align_val_t{align});
    }
    void arm(std::size_t allow) noexcept {
        armed_ = true;
        allow_ = allow;
    }
    void disarm() noexcept { armed_ = false; }
    long live_ = 0;

   private:
    bool armed_ = false;
    std::size_t allow_ = 0;
};

/** @brief The object is drawn from the graph's injected source and refused by value. */
void test_drawn_from_the_seam() {
    std::printf("drawn from the injected source, refused by value:\n");
    gate_source_t src;
    graph_t g(src);
    const long base = src.live_;
    for (std::size_t allow = 0; allow < 2; ++allow) {
        src.arm(allow);
        const auto r = g.make_path("/seam/x");
        src.disarm();
        check(!r.has_value() && r.error() == status_t::BACKPRESSURE && src.live_ == base,
              "an exhausted source answers BACKPRESSURE and keeps nothing");
    }
    {
        auto p = g.make_path("/seam/x");
        check(p.has_value() && src.live_ == base + 1, "with room the object is one block");
        auto moved = std::move(*p);
        check(p->empty() && !moved.empty() && src.live_ == base + 1, "a move takes the block");
    }
    check(src.live_ == base, "destroying the object gives the block back");
}

}  // namespace

int main() {
    test_from_string();
    test_stale_falls_back();
    test_rom_and_refusals();
    test_chain_cache();
    test_chain_race();
    test_element0_race();
    test_drawn_from_the_seam();
    return tr::testing::summary("path_object");
}
