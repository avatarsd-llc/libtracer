/**
 * @file
 * @brief Terminus arena decoder test (ADR-0041).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Three pillars:
 *   (1) equivalence — for EVERY conformance vector under
 *       tests/conformance/vectors/v1/, decode() and decode_into() must agree
 *       node-for-node (type, opt, body bytes, structure) and error-for-error,
 *       so the arena is gated by the same vectors as the tree decoder;
 *   (2) the ADR-0041 span contract — `wire` excludes the trailer, `body` spans
 *       alias the input buffer, `end` encodes the pre-order subtree, and
 *       `canonical_path` is byte-identical to path_key for canonical PATHs;
 *   (3) memory — a typical frame decodes entirely inside a stack-buffer
 *       monotonic_buffer_resource with a null upstream (zero heap anywhere),
 *       and every rejection branch (truncation at each boundary, reserved
 *       bits, type 0x00, CRC fail both widths, trailing bytes) returns the
 *       same err_t as decode().
 */

#include "libtracer/tlv_arena.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/tlv_emit.hpp"
#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;
using namespace tr::wire;

using tr::testing::check;

std::vector<std::byte> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// ---- equivalence: the arena and the tree decoder must agree ----------------

/**
 * @brief Compare one tlv_t subtree against the arena subtree rooted at `idx`; returns one past the
 *        subtree (the node's `end`) or 0 on mismatch.
 */
std::uint32_t match_subtree(const tlv_t& t, const tlv_arena_t& a, std::uint32_t idx) {
    const arena_tlv_t& n = a[idx];
    if (n.type != t.type || n.opt != t.opt) return 0;

    // `wire` = header + body, trailer excluded, and it aliases the input.
    const std::size_t header = n.opt.ll ? 6u : 4u;
    if (n.wire.size() != header + n.body.size()) return 0;
    if (n.body.data() != n.wire.data() + header) return 0;

    if (!n.opt.pl) {
        if (!std::ranges::equal(n.body, t.payload)) return 0;
        return n.end == idx + 1 ? n.end : 0;
    }
    std::uint32_t child = idx + 1;
    for (const tlv_t& tc : t.children) {
        if (child >= n.end) return 0;  // fewer arena children than tree children
        child = match_subtree(tc, a, child);
        if (child == 0) return 0;
    }
    return child == n.end ? n.end : 0;  // extra arena children ⇒ mismatch
}

/** @brief A fresh nothrow source per decode — the heap, so nothing is ever refused. */
tr::mem::block_source_t& fresh_heap_resource() { return tr::mem::heap_source(); }

/**
 * @brief Run both decoders over `bytes`; check same accept/reject, same error, and — on accept —
 *        node-for-node agreement.
 */
bool equivalent(std::span<const std::byte> bytes, std::string_view label) {
    const auto tree = decode(bytes);
    auto& mr = fresh_heap_resource();
    const auto arena = decode_into(bytes, mr);
    if (tree.has_value() != arena.has_value()) {
        std::printf("    [%.*s] accept/reject disagree\n", static_cast<int>(label.size()),
                    label.data());
        return false;
    }
    if (!tree) {
        if (tree.error() != arena.error()) {
            std::printf("    [%.*s] error codes disagree\n", static_cast<int>(label.size()),
                        label.data());
            return false;
        }
        return true;
    }
    return match_subtree(*tree, *arena, 0) == arena->size();
}

// ---- builders ---------------------------------------------------------------

tlv_t make_value(std::span<const std::byte> payload) {
    tlv_t v;
    v.type = type_t::VALUE;
    v.payload = payload;
    return v;
}

tlv_t make_path(std::vector<tlv_t> names) {
    tlv_t p;
    p.type = type_t::PATH;
    p.opt.pl = true;
    p.children = std::move(names);
    return p;
}

tlv_t make_name(std::string_view s) {
    tlv_t n;
    n.type = type_t::NAME;
    n.payload = std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
    return n;
}

tlv_t nested(int leaf_depth) {
    tlv_t node;
    if (leaf_depth == 0) {
        node.type = type_t::VALUE;
        return node;
    }
    node.type = type_t::FWD;
    node.opt.pl = true;
    node.children.push_back(nested(leaf_depth - 1));
    return node;
}

}  // namespace

int main() {
    std::printf("tlv_arena decode_into (ADR-0041):\n");

    // (1) Equivalence over every conformance vector.
    {
        std::size_t count = 0;
        bool all = true;
        for (const auto& entry : fs::recursive_directory_iterator(LIBTRACER_VECTORS_DIR)) {
            if (entry.path().filename() != "input.bin") continue;
            ++count;
            const std::vector<std::byte> bytes = read_file(entry.path());
            if (!equivalent(bytes, entry.path().parent_path().filename().string())) all = false;
        }
        std::printf("  (%zu vectors)\n", count);
        check(count > 0, "conformance vectors found");
        check(all, "decode == decode_into on every conformance vector");
    }

    // (2a) Trailer-sliced `wire` span + opt bits retained, all four trailer shapes.
    {
        const std::array<std::byte, 3> payload{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
        bool all = true;
        for (const bool cw : {false, true}) {
            for (const bool ts : {false, true}) {
                for (const bool tf : {false, true}) {
                    if (tf && !ts) continue;
                    tlv_t v = make_value(payload);
                    v.opt.cr = true;
                    v.opt.cw = cw;
                    v.opt.ts = ts;
                    v.opt.tf = tf;
                    // A claimed timestamp must CARRY a value since #1109 (encode refuses the
                    // old silent-zero shape), so the TS shapes stamp a real one, in the form
                    // the TF bit names.
                    if (ts) {
                        v.trailer.emplace();
                        v.trailer->ts =
                            tr::wire::timestamp_t{.relative = tf, .value = tf ? -42 : 123456789};
                    }
                    const std::vector<std::byte> bytes = encode(v);
                    auto& mr = fresh_heap_resource();
                    const auto arena = decode_into(bytes, mr);
                    if (!arena) {
                        all = false;
                        continue;
                    }
                    const arena_tlv_t& n = arena->root();
                    const std::size_t trailer = (ts ? (tf ? 4u : 8u) : 0u) + (cw ? 2u : 4u);
                    all = all && n.wire.size() == bytes.size() - trailer &&
                          n.wire.data() == bytes.data() && n.opt == v.opt &&
                          std::ranges::equal(n.body, payload) && equivalent(bytes, "trailer shape");
                }
            }
        }
        check(all, "wire span excludes the trailer for every CRC/TS shape");
    }

    // (2b) Pre-order layout + end indices + sibling iteration on a FWD-like tree.
    {
        // FWD{ VALUE "op", PATH{a,b}, VALUE payload }
        tlv_t fwd;
        fwd.type = type_t::FWD;
        fwd.opt.pl = true;
        const std::array<std::byte, 1> op{std::byte{0x00}};
        fwd.children.push_back(make_value(op));
        fwd.children.push_back(make_path({make_name("a"), make_name("b")}));
        const std::array<std::byte, 2> pl{std::byte{0x01}, std::byte{0x02}};
        fwd.children.push_back(make_value(pl));

        const std::vector<std::byte> bytes = encode(fwd);
        auto& mr = fresh_heap_resource();
        const auto arena = decode_into(bytes, mr);
        check(arena.has_value(), "FWD-like tree decodes");
        if (arena) {
            // Pre-order: 0 FWD, 1 VALUE, 2 PATH, 3 NAME a, 4 NAME b, 5 VALUE.
            check(arena->size() == 6, "pre-order node count");
            check(arena->root().end == 6 && (*arena)[2].end == 5 && (*arena)[3].end == 4,
                  "end indices encode the subtrees");
            std::vector<std::uint32_t> kids;
            for (std::uint32_t j = tlv_arena_t::first_child(0); j < arena->root().end;
                 j = arena->next_sibling(j))
                kids.push_back(j);
            check(kids == std::vector<std::uint32_t>({1, 2, 5}), "sibling iteration walks 1,2,5");
            check(equivalent(bytes, "fwd tree"), "FWD tree equivalent to decode()");
        }
    }

    // (2c) canonical_path: body is byte-identical to path_key ⇒ flag true.
    {
        const tlv_t path = make_path({make_name("net"), make_name("ws"), make_name("peer1")});
        const std::vector<std::byte> bytes = encode(path);
        auto& mr = fresh_heap_resource();
        const auto arena = decode_into(bytes, mr);
        const auto tree = decode(bytes);
        check(arena && arena->root().canonical_path, "canonical PATH flagged");
        if (arena && tree) {
            const auto key = path_key(*tree);
            check(key.has_value(), "a canonical PATH yields a key (all children are NAMEs)");
            check(key && std::ranges::equal(arena->root().body, *key),
                  "canonical PATH body == path_key bytes");
        }
    }

    // (2d) Non-canonical PATHs fall back: LL-widened NAME, trailer-carrying
    // NAME, structured child, and a non-PATH node never sets the flag.
    {
        tlv_t ll_name = make_name("x");
        ll_name.opt.ll = true;
        const std::vector<std::byte> b1 = encode(make_path({make_name("a"), ll_name}));

        tlv_t crc_name = make_name("y");
        crc_name.opt.cr = true;
        const std::vector<std::byte> b2 = encode(make_path({crc_name}));

        tlv_t sub_path = make_path({make_name("z")});
        const std::vector<std::byte> b3 = encode(make_path({sub_path}));

        tlv_t fwd_names;  // canonical-shaped children under a non-PATH parent
        fwd_names.type = type_t::FWD;
        fwd_names.opt.pl = true;
        fwd_names.children.push_back(make_name("n"));
        const std::vector<std::byte> b4 = encode(fwd_names);

        bool all = true;
        for (const auto* b : {&b1, &b2, &b3, &b4}) {
            auto& mr = fresh_heap_resource();
            const auto arena = decode_into(*b, mr);
            all = all && arena && !arena->root().canonical_path && equivalent(*b, "non-canonical");
        }
        check(all, "LL/trailer/structured children and non-PATH ⇒ canonical_path false");

        // Nested canonical PATH inside a FWD is still flagged on the PATH node.
        tlv_t fwd;
        fwd.type = type_t::FWD;
        fwd.opt.pl = true;
        fwd.children.push_back(make_path({make_name("a")}));
        const std::vector<std::byte> b5 = encode(fwd);
        auto& mr = fresh_heap_resource();
        const auto arena = decode_into(b5, mr);
        check(arena && !arena->root().canonical_path && (*arena)[1].canonical_path,
              "nested PATH inside FWD flagged on the PATH node only");
    }

    // (2e) Depth is receiver-resource-bounded (RFC-0006): a frame far deeper
    // than the old cap of 32 decodes on a heap-backed resource, and the arena
    // still agrees with decode() node-for-node.
    {
        const std::vector<std::byte> deep_bytes = encode(nested(100));
        auto& mr = fresh_heap_resource();
        check(decode_into(deep_bytes, mr).has_value() && equivalent(deep_bytes, "depth 100"),
              "deep nesting (100 levels) decodes, arena == tree");
    }

    // (3a) Every rejection branch returns the same error as decode().
    {
        const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2}, std::byte{3}};
        tlv_t crc16 = make_value(payload);
        crc16.opt.cr = true;
        crc16.opt.cw = true;
        tlv_t crc32 = make_value(payload);
        crc32.opt.cr = true;

        std::vector<std::vector<std::byte>> cases;
        cases.push_back({});                                  // empty
        cases.push_back({std::byte{0x01}, std::byte{0x00}});  // < 4 bytes
        {
            std::vector<std::byte> b;  // LL header cut at 4 bytes
            tr::wire::emit_tlv(b, type_t::VALUE, opt_t{.ll = true}, {});
            b.resize(4);
            cases.push_back(std::move(b));
        }
        {
            std::vector<std::byte> b = encode(make_value(payload));  // body cut
            b.pop_back();
            cases.push_back(std::move(b));
        }
        {
            std::vector<std::byte> b = encode(crc32);  // trailer cut
            b.pop_back();
            cases.push_back(std::move(b));
        }
        cases.push_back(
            {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}});  // type 0x00
        cases.push_back({std::byte{0x01}, std::byte{0x81}, std::byte{0x00},
                         std::byte{0x00}});  // reserved opt bits
        {
            std::vector<std::byte> b = encode(crc16);  // CRC16 corrupted
            b.back() ^= std::byte{0xFF};
            cases.push_back(std::move(b));
        }
        {
            std::vector<std::byte> b = encode(crc32);  // CRC32 corrupted
            b.back() ^= std::byte{0xFF};
            cases.push_back(std::move(b));
        }
        {
            std::vector<std::byte> b = encode(make_value(payload));  // trailing bytes
            b.push_back(std::byte{0x00});
            cases.push_back(std::move(b));
        }
        {
            // Truncated CHILD inside a structured parent (the region-bounded parse).
            std::vector<std::byte> inner = encode(make_value(payload));
            inner.pop_back();
            std::vector<std::byte> b;
            tr::wire::emit_tlv(b, type_t::FWD, opt_t{.pl = true}, inner);
            cases.push_back(std::move(b));
        }
        bool all = true;
        std::size_t i = 0;
        for (const auto& c : cases) {
            auto& mr = fresh_heap_resource();
            const auto arena = decode_into(c, mr);
            if (arena.has_value() || !equivalent(c, "rejection " + std::to_string(i))) all = false;
            ++i;
        }
        check(all, "all rejection branches match decode() (11 cases)");
    }

    // (3b) A typical terminus frame decodes with ZERO allocation outside a
    // 4 KiB stack buffer — a null upstream refuses any spill, so a decode that
    // needed one would come back TLV_NESTING_TOO_DEEP rather than reach the heap.
    {
        tlv_t fwd;
        fwd.type = type_t::FWD;
        fwd.opt.pl = true;
        const std::array<std::byte, 1> op{std::byte{0x00}};
        fwd.children.push_back(make_value(op));
        fwd.children.push_back(make_path({make_name("net"), make_name("ws"), make_name("peer")}));
        fwd.children.push_back(make_path({make_name("back")}));
        const std::array<std::byte, 8> pl{};
        fwd.children.push_back(make_value(pl));
        const std::vector<std::byte> bytes = encode(fwd);

        alignas(std::max_align_t) std::array<std::byte, 4096> buf;
        tr::mem::bump_source_t mr(buf, tr::mem::null_source());
        const auto arena = decode_into(bytes, mr);
        check(arena.has_value() && arena->size() == 9,
              "typical FWD decodes inside a 4KiB stack buffer (null upstream)");
    }

    // (3c) THE #588 CASE: an exhausted source REJECTS, it does not abort.
    //
    // `decode_into` runs on the wire RX path behind no ACL, and a peer picks both the
    // nesting depth and the node count. Before the block seam, all three draws here — the
    // node array, the sink's open-node stack, and the walk stack's spill past its 8 inline
    // slots — went through a throwing `std::pmr` allocate, which on a -fno-exceptions node
    // is the link-wrapped `abort()` stub. Each of the three is probed separately, because a
    // fix that guards only one leaves the others reachable.
    //
    // Every case here TERMINATES the process if the guard is missing, so the assertion
    // being reached at all is half the result.
    {
        // A source that serves exactly `budget` blocks, then refuses forever — the
        // "exhausted at the Nth allocation" injection #588 asked for.
        struct budget_source_t final : tr::mem::block_source_t {
            explicit budget_source_t(int budget) noexcept
                : tr::mem::block_source_t("budget"), left_(budget) {}
            [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
                if (left_-- <= 0) return nullptr;
                return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
            }
            void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
                ::operator delete(p, bytes, std::align_val_t{align});
            }
            int left_;
        };

        // Deeper than the 8 inline walk-stack slots, so the spill is genuinely entered.
        const std::vector<std::byte> deep = encode(nested(24));
        check(decode_into(deep, tr::mem::heap_source()).has_value(),
              "the 24-deep frame decodes fine on an unbounded source (the probe is valid)");

        // Zero blocks: the very first draw (the node array's reserve) is refused.
        budget_source_t none(0);
        const auto r0 = decode_into(deep, none);
        check(!r0.has_value() && r0.error() == err_t::TLV_NESTING_TOO_DEEP,
              "0-block source => TLV_NESTING_TOO_DEEP (node array refused, no abort)");

        // One block: the node array gets its reserve, the sink's open-node stack is refused.
        budget_source_t one(1);
        const auto r1 = decode_into(deep, one);
        check(!r1.has_value() && r1.error() == err_t::TLV_NESTING_TOO_DEEP,
              "1-block source => TLV_NESTING_TOO_DEEP (sink stack refused, no abort)");

        // Enough for both containers' first blocks but not for the walk stack's spill.
        budget_source_t two(2);
        const auto r2 = decode_into(deep, two);
        check(!r2.has_value() && r2.error() == err_t::TLV_NESTING_TOO_DEEP,
              "2-block source => TLV_NESTING_TOO_DEEP (walk spill refused, no abort)");

        // The same shape through the composition a bounded node actually uses: a small
        // stack buffer whose upstream serves nothing.
        alignas(std::max_align_t) std::array<std::byte, 64> tiny;
        tr::mem::bump_source_t bounded(tiny, tr::mem::null_source());
        const auto rb = decode_into(deep, bounded);
        check(!rb.has_value() && rb.error() == err_t::TLV_NESTING_TOO_DEEP,
              "a 64 B bump over a null upstream => TLV_NESTING_TOO_DEEP, never an abort");
    }

    return tr::testing::summary("tlv_arena");
}
