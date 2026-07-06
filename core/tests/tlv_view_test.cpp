/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Tests for the lazy rope-backed decode view (ADR-0053): tlv_view_t over a
 * scatter-gather rope must (a) agree with the eager decoder node-for-node when
 * fully walked, (b) be actually LAZY — siblings of a corrupt TLV deliver, the
 * corrupt one fails only its own verify(), bytes are shared not copied — and
 * (c) keep its links' segments alive past the source rope (owning tier).
 */

#include "libtracer/tlv_view.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// Owns the per-link byte copies and the rope viewing them (borrowed links).
struct split_rope_t {
    std::vector<std::vector<std::byte>> parts;
    tr::view::rope_t rope;
};

split_rope_t split_into(const std::vector<std::byte>& flat, const std::vector<std::size_t>& cuts) {
    split_rope_t out;
    out.parts.reserve(cuts.size() + 1);
    std::size_t prev = 0;
    for (const std::size_t c : cuts) {
        out.parts.emplace_back(flat.begin() + static_cast<std::ptrdiff_t>(prev),
                               flat.begin() + static_cast<std::ptrdiff_t>(c));
        prev = c;
    }
    out.parts.emplace_back(flat.begin() + static_cast<std::ptrdiff_t>(prev), flat.end());
    for (auto& p : out.parts) {
        out.rope.append(tr::view::view_t::over(tr::view::borrow(std::span<std::byte>(p))));
    }
    return out;
}

std::vector<std::byte> rope_bytes(const tr::view::rope_t& r) {
    std::vector<std::byte> out;
    r.walk([&out](std::span<const std::byte> s) { out.insert(out.end(), s.begin(), s.end()); });
    return out;
}

// Recursively compare a lazy view against the eager decode of the same bytes:
// header facts, payload bytes, child count/order, and verify() at every node.
bool lazy_equals_eager(const tr::wire::tlv_view_t& v, const tr::wire::tlv_t& t) {
    if (v.type() != t.type || !(v.opt() == t.opt)) return false;
    if (!v.verify().has_value()) return false;
    if (!v.structured()) {
        const std::vector<std::byte> body = rope_bytes(v.body());
        return std::ranges::equal(body, t.payload);
    }
    auto it = v.children();
    std::size_t i = 0;
    while (true) {
        auto nx = it.next();
        if (!nx) return false;
        if (!*nx) break;
        if (i >= t.children.size()) return false;
        if (!lazy_equals_eager(**nx, t.children[i])) return false;
        ++i;
    }
    return i == t.children.size();
}

// ---- Frame builders (via the owning codec, canonical bytes) ----

std::vector<std::byte> pattern_payload(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::byte>(seed + i);
    return p;
}

tr::wire::tlv_t opaque(std::span<const std::byte> pay, bool crc32 = false, bool crc16 = false,
                       bool ts = false) {
    tr::wire::tlv_t t;
    t.type = tr::wire::type_t::VALUE;
    t.payload = pay;
    if (crc32) t.opt.cr = true;
    if (crc16) {
        t.opt.cr = true;
        t.opt.cw = true;
    }
    if (ts) {
        t.opt.ts = true;
        tr::wire::trailer_t tr;
        tr.ts = tr::wire::timestamp_t{.relative = false, .value = 0x1122334455667788};
        t.trailer = tr;
    }
    return t;
}

// ---- Tests ----

void test_full_lazy_walk_equals_decode() {
    const std::vector<std::byte> pay0 = pattern_payload(9, 0x10);
    const std::vector<std::byte> pay1 = pattern_payload(21, 0x40);
    const std::vector<std::byte> pay2 = pattern_payload(0, 0x00);

    // A structured frame: [opaque, opaque+crc32, opaque+crc16+ts, nested struct].
    tr::wire::tlv_t root;
    root.type = tr::wire::type_t::PATH;
    root.opt.pl = true;
    root.children.push_back(opaque(pay0));
    root.children.push_back(opaque(pay1, /*crc32=*/true));
    root.children.push_back(opaque(pay2, false, /*crc16=*/true, /*ts=*/true));
    tr::wire::tlv_t nested;
    nested.type = tr::wire::type_t::PATH;
    nested.opt.pl = true;
    nested.children.push_back(opaque(pay0, /*crc32=*/true));
    root.children.push_back(nested);

    const std::vector<std::byte> flat = tr::wire::encode(root);
    const auto eager = tr::wire::decode(flat);
    if (!eager) {
        check(false, "full lazy walk == decode (eager decode failed?)");
        return;
    }

    // Every 2-link split — the lazy walk must agree at every cut, including
    // cuts through child headers and trailers.
    bool all = true;
    for (std::size_t cut = 0; cut < flat.size(); ++cut) {
        const std::vector<std::size_t> cuts =
            cut == 0 ? std::vector<std::size_t>{} : std::vector<std::size_t>{cut};
        split_rope_t sr = split_into(flat, cuts);
        const auto v = tr::wire::tlv_view_t::over(sr.rope);
        if (!v || !lazy_equals_eager(*v, *eager)) {
            std::printf("    (diverged at cut=%zu)\n", cut);
            all = false;
            break;
        }
    }
    check(all, "full lazy walk == decode at every 2-link cut");
}

void test_partial_delivery_semantics() {
    // Three children; child[1] carries a CRC-32C trailer. Corrupt one payload
    // byte of child[1]: the EAGER decoder rejects the whole frame, the lazy
    // tier delivers children 0 and 2 intact and fails only child[1]'s verify()
    // — the ADR-0053 §4 partial-consumption semantics.
    const std::vector<std::byte> pay0 = pattern_payload(5, 0x50);
    const std::vector<std::byte> pay1 = pattern_payload(8, 0x70);
    const std::vector<std::byte> pay2 = pattern_payload(6, 0x90);

    tr::wire::tlv_t root;
    root.type = tr::wire::type_t::PATH;
    root.opt.pl = true;
    root.children.push_back(opaque(pay0));
    root.children.push_back(opaque(pay1, /*crc32=*/true));
    root.children.push_back(opaque(pay2));

    std::vector<std::byte> flat = tr::wire::encode(root);
    // child[1]'s first payload byte: root header (4) + child0 (4+5) + child1 header (4).
    const std::size_t corrupt_at = 4 + (4 + 5) + 4;
    flat[corrupt_at] ^= std::byte{0xFF};

    check(!tr::wire::decode(flat).has_value(), "eager decode rejects the corrupted frame");

    split_rope_t sr = split_into(flat, {corrupt_at});  // cut AT the corruption for spice
    auto v = tr::wire::tlv_view_t::over(sr.rope);
    if (!v) {
        check(false, "lazy over() accepts (bounds are intact)");
        return;
    }
    check(v->verify().has_value(), "root (no CRC of its own) verifies");

    auto it = v->children();
    auto c0 = it.next();
    auto c1 = it.next();
    auto c2 = it.next();
    auto end = it.next();
    const bool yielded = c0 && *c0 && c1 && *c1 && c2 && *c2 && end && !*end;
    check(yielded, "all three children are yielded despite the corruption");
    if (!yielded) return;

    check((*c0)->verify().has_value(), "sibling child[0] verifies clean");
    check((*c2)->verify().has_value(), "sibling child[2] verifies clean");
    const auto bad = (*c1)->verify();
    check(!bad.has_value() && bad.error() == tr::wire::err_t::FRAME_CRC_FAIL,
          "child[1] fails ITS OWN verify() with FRAME_CRC_FAIL");
    check(std::ranges::equal(rope_bytes((*c0)->body()), pay0) &&
              std::ranges::equal(rope_bytes((*c2)->body()), pay2),
          "sibling payloads deliver byte-intact");
}

void test_bounds_and_grammar_errors() {
    const std::vector<std::byte> pay = pattern_payload(7, 0x30);
    std::vector<std::byte> flat = tr::wire::encode(opaque(pay));

    {  // trailing byte after the frame -> over() rejects (bounds anchor)
        std::vector<std::byte> f = flat;
        f.push_back(std::byte{0xEE});
        split_rope_t sr = split_into(f, {3});
        const auto v = tr::wire::tlv_view_t::over(sr.rope);
        check(!v.has_value() && v.error() == tr::wire::err_t::FRAME_INVALID,
              "trailing bytes -> over() FRAME_INVALID");
    }
    {  // truncated frame -> over() rejects
        std::vector<std::byte> f(flat.begin(), flat.end() - 2);
        split_rope_t sr = split_into(f, {2});
        const auto v = tr::wire::tlv_view_t::over(sr.rope);
        check(!v.has_value() && v.error() == tr::wire::err_t::FRAME_TRUNCATED,
              "truncation -> over() FRAME_TRUNCATED");
    }
    {  // a child whose type is 0x00 -> the error surfaces at next(), and poisons
        tr::wire::tlv_t root;
        root.type = tr::wire::type_t::PATH;
        root.opt.pl = true;
        root.children.push_back(opaque(pay));
        std::vector<std::byte> f = tr::wire::encode(root);
        f[4] = std::byte{0x00};  // child header's type byte
        split_rope_t sr = split_into(f, {6});
        auto v = tr::wire::tlv_view_t::over(sr.rope);
        if (!v) {
            check(false, "over() accepts (root bounds are intact)");
        } else {
            auto it = v->children();
            const auto c = it.next();
            const bool first = !c.has_value() && c.error() == tr::wire::err_t::FRAME_INVALID;
            const auto again = it.next();
            check(first && !again.has_value() && again.error() == c.error(),
                  "bad child header -> next() FRAME_INVALID, iterator poisoned");
        }
    }
}

void test_zero_copy_and_ownership() {
    const std::vector<std::byte> pay = pattern_payload(11, 0x60);
    const std::vector<std::byte> flat = tr::wire::encode(opaque(pay));

    {  // zero-copy: the body subrope's bytes ARE the source buffer's bytes
        split_rope_t sr = split_into(flat, {6});  // cut inside the payload
        const auto v = tr::wire::tlv_view_t::over(sr.rope);
        if (!v) {
            check(false, "over() accepts the split frame");
        } else {
            const tr::view::rope_t body = v->body();
            bool same_memory = body.link_count() == 2;
            if (same_memory) {
                same_memory = body.links()[0].bytes().data() == sr.parts[0].data() + 4 &&
                              body.links()[1].bytes().data() == sr.parts[1].data();
            }
            check(same_memory, "body() links alias the source buffers (no copy)");
        }
    }
    {  // ownership: a child view keeps its segment alive past the source rope
        tr::view::view_t owned;
        {
            split_rope_t sr = split_into(flat, {});
            owned = sr.rope.flatten();  // one owning (heap-segment) copy
        }  // sr.parts (the borrowed buffers) die here; `owned` does not
        std::vector<std::byte> got;
        {
            const auto v = tr::wire::tlv_view_t::over(tr::view::rope_t{owned});
            if (v) got = rope_bytes(v->body());
        }  // the tlv_view_t (and its subropes) released their refcounts here
        check(std::ranges::equal(got, pay), "view reads through refcounted segment ownership");
    }
}

void test_materialize_and_timestamp() {
    const std::vector<std::byte> pay = pattern_payload(13, 0x20);
    const std::vector<std::byte> flat = tr::wire::encode(opaque(pay, true, false, /*ts=*/true));
    const auto eager = tr::wire::decode(flat);

    split_rope_t sr = split_into(flat, {5, flat.size() - 3});  // cuts in payload + trailer
    const auto v = tr::wire::tlv_view_t::over(sr.rope);
    if (!v || !eager) {
        check(false, "materialize/timestamp precondition (over/decode ok)");
        return;
    }
    const auto ts = v->timestamp();
    check(ts && eager->trailer && eager->trailer->ts && *ts == *eager->trailer->ts,
          "lazy timestamp() == eager trailer (stitched across links)");

    const auto m = v->materialize();
    check(m.has_value() && tr::wire::equal(m->root, *eager),
          "materialize() == decode (the one explicit copy)");
}

}  // namespace

int main() {
    std::printf("tlv_view_t (lazy rope-backed decode view, ADR-0053):\n");
    test_full_lazy_walk_equals_decode();
    test_partial_delivery_semantics();
    test_bounds_and_grammar_errors();
    test_zero_copy_and_ownership();
    test_materialize_and_timestamp();
    if (g_failures != 0) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
