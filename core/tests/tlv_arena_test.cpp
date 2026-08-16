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
#include <array>
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
#include "libtracer/packed_path.hpp"
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

/**
 * @brief A packed `PATH` over @p segs (RFC-0018): `opt.PL = 0`, body = `[u8 len][bytes]`
 *        records. The body bytes are OWNED by @p storage, which must outlive the returned TLV.
 */
tlv_t make_path(std::span<const std::string_view> segs, std::vector<std::byte>& storage) {
    storage.clear();
    for (const std::string_view seg : segs) {
        const bool ok = tr::wire::emit_path_segment(storage, seg);
        (void)ok;
    }
    tlv_t p;
    p.type = type_t::PATH;
    p.payload = std::span<const std::byte>(storage);
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
        std::vector<std::byte> ab;
        const std::array<std::string_view, 2> ab_segs{"a", "b"};
        fwd.children.push_back(make_path(ab_segs, ab));
        const std::array<std::byte, 2> pl{std::byte{0x01}, std::byte{0x02}};
        fwd.children.push_back(make_value(pl));

        const std::vector<std::byte> bytes = encode(fwd);
        auto& mr = fresh_heap_resource();
        const auto arena = decode_into(bytes, mr);
        check(arena.has_value(), "FWD-like tree decodes");
        if (arena) {
            // Pre-order: 0 FWD, 1 VALUE, 2 PATH (opaque under RFC-0018 — its packed body
            // is not a child run, so it contributes no per-segment nodes), 3 VALUE.
            check(arena->size() == 4, "pre-order node count");
            check(arena->root().end == 4 && (*arena)[2].end == 3,
                  "end indices encode the subtrees");
            std::vector<std::uint32_t> kids;
            for (std::uint32_t j = tlv_arena_t::first_child(0); j < arena->root().end;
                 j = arena->next_sibling(j))
                kids.push_back(j);
            check(kids == std::vector<std::uint32_t>({1, 2, 3}), "sibling iteration walks 1,2,3");
            check(equivalent(bytes, "fwd tree"), "FWD tree equivalent to decode()");
        }
    }

    // (2c) The packed PATH body IS the key, unconditionally (RFC-0018 §4 — the
    // `canonical_path` flag and its re-emit fallback are gone with the encoding).
    {
        std::vector<std::byte> body;
        const std::array<std::string_view, 3> segs{"net", "ws", "peer1"};
        const tlv_t path = make_path(segs, body);
        const std::vector<std::byte> bytes = encode(path);
        auto& mr = fresh_heap_resource();
        const auto arena = decode_into(bytes, mr);
        const auto tree = decode(bytes);
        check(arena && arena->size() == 1 && !arena->root().opt.pl,
              "a packed PATH decodes as ONE opaque node (opt.PL = 0)");
        if (arena && tree) {
            const auto key = path_key(*tree);
            check(key.has_value(), "a packed PATH yields a key");
            check(key && std::ranges::equal(arena->root().body, *key),
                  "packed PATH body == path_key bytes — the span-alias, guaranteed");
        }
    }

    // (2d) What the key-context walk refuses (RFC-0018 §5 / §5.4): a ragged record, the
    // `len == 0` escape, and a STRUCTURED (`opt.PL = 1`) PATH — the pre-RFC spelling.
    {
        const auto bytes_of = [](std::initializer_list<int> v) {
            std::vector<std::byte> out;
            for (const int b : v) out.push_back(static_cast<std::byte>(b));
            return out;
        };
        // A length that runs past the body's end.
        const std::vector<std::byte> ragged = bytes_of({0x03, 'a', 'b'});
        // The escape: `00 <kind=0x16> <len=4> <u32>` — admissible in a frame path, and a key
        // is not a frame path.
        const std::vector<std::byte> escape =
            bytes_of({0x03, 'n', 'e', 't', 0x00, 0x16, 0x04, 1, 0, 2, 0});
        bool refused = true;
        for (const auto* b : {&ragged, &escape}) {
            tlv_t p;
            p.type = type_t::PATH;
            p.payload = std::span<const std::byte>(*b);
            const std::vector<std::byte> wire_bytes = encode(p);
            const auto tree = decode(wire_bytes);
            refused = refused && tree && !path_key(*tree).has_value();
        }
        check(refused, "ragged framing and the len==0 escape are refused in key context");

        // The pre-RFC structured spelling is no longer a key either.
        tlv_t structured;
        structured.type = type_t::PATH;
        structured.opt.pl = true;
        structured.children.push_back(make_name("a"));
        const std::vector<std::byte> b_struct = encode(structured);
        const auto struct_tree = decode(b_struct);
        check(struct_tree && !path_key(*struct_tree).has_value(),
              "a structured (opt.PL=1) PATH is not a key");

        // A packed PATH nested inside a FWD stays one opaque node.
        std::vector<std::byte> a_body;
        const std::array<std::string_view, 1> a_segs{"a"};
        tlv_t fwd;
        fwd.type = type_t::FWD;
        fwd.opt.pl = true;
        fwd.children.push_back(make_path(a_segs, a_body));
        const std::vector<std::byte> b5 = encode(fwd);
        auto& mr = fresh_heap_resource();
        const auto arena = decode_into(b5, mr);
        check(
            arena && arena->size() == 2 && (*arena)[1].type == type_t::PATH && !(*arena)[1].opt.pl,
            "a packed PATH inside a FWD is one opaque child node");
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
        std::vector<std::byte> dst_body;
        std::vector<std::byte> src_body;
        const std::array<std::string_view, 3> dst_segs{"net", "ws", "peer"};
        const std::array<std::string_view, 1> src_segs{"back"};
        fwd.children.push_back(make_path(dst_segs, dst_body));
        fwd.children.push_back(make_path(src_segs, src_body));
        const std::array<std::byte, 8> pl{};
        fwd.children.push_back(make_value(pl));
        const std::vector<std::byte> bytes = encode(fwd);

        alignas(std::max_align_t) std::array<std::byte, 4096> buf;
        tr::mem::bump_source_t mr(buf, tr::mem::null_source());
        const auto arena = decode_into(bytes, mr);
        // 0 FWD, 1 VALUE op, 2 PATH dst (opaque), 3 PATH src (opaque), 4 VALUE payload —
        // the two PATHs contribute no per-segment nodes under RFC-0018.
        check(arena.has_value() && arena->size() == 5,
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
