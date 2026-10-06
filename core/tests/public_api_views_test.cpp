/**
 * @file
 * @brief The view / caller-storage spellings of the public API (#1781): byte-identical to the
 *        `std` spellings they replace, and refused by value instead of thrown.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0083 Decision 11: no owning std type crosses core's public API. This is the expand step:
 * every new spelling sits beside the `std::vector` / `std::string` one it replaces, so this
 * test pins the two equal, byte for byte, and then pins the new one's refusal arm over a
 * source that refuses (`null_source()`), which the `std` spelling cannot express:
 *
 * 1. the wire emitters into `mem::bytes_t` (`tlv_emit`, `packed_path`, `path_label`,
 *    `path_element`, `byteorder`, `batch`);
 * 2. `wire::encode(tlv, bytes_t&)` and `wire::path_key`'s view;
 * 3. `rope_t` / `value_ref_t::to_iovec` into a caller span;
 * 4. `encode_acl` / `parse_acl` into core containers, and `conn_spec_t::bytes(bytes_t&)`;
 * 5. `graph_t::read_subscribers` / `drain_unflushed` into core arrays;
 * 6. the `subject_lookup` hook, which writes the subject token into the gate's own frame, and
 *    the returning `subject_resolver`, which still gates through the adapter.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/batch.hpp"
#include "libtracer/conn_spec.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::testing::check;
using tr::wire::opt_t;
using tr::wire::type_t;
namespace mem = tr::mem;
namespace wire = tr::wire;

/** @brief @p s's bytes as a span. */
[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/** @brief True iff @p a holds exactly @p b's bytes. */
[[nodiscard]] bool same(const mem::bytes_t& a, const std::vector<std::byte>& b) noexcept {
    return std::ranges::equal(mem::as_span(a), b);
}

void test_emitters_match() {
    std::printf("wire emitters into bytes_t match the vector forms:\n");
    std::vector<std::byte> v;
    mem::bytes_t b(mem::heap_source());
    const std::vector<std::byte> big(0x10010, std::byte{0x5A});  // past 0xFFFF: the LL widen

    wire::emit_header(v, type_t::VALUE, opt_t{}, 3);
    wire::emit_tlv(v, type_t::POINT, opt_t{.pl = true}, bytes_of("abc"));
    wire::emit_tlv(v, type_t::VALUE, opt_t{.ts = true}, big);  // trailer bit cleared, LL set
    wire::emit_name(v, "sensor");
    wire::emit_name(v, bytes_of("raw"));
    wire::emit_value_le(v, std::uint32_t{0xA1B2C3D4}, 3);
    wire::emit_trailer_ts(v, false, 1'700'000'000'000'000'000);
    wire::emit_trailer_ts(v, true, -42);
    const std::array<wire::path_ref_element_t, 2> refs{
        {{.index = 7, .generation = 1}, {.index = 9, .generation = 3}}};
    check(wire::emit_path_ref(v, refs), "vector PATH_REF emitted");
    check(wire::emit_path_segment(v, std::string_view{"seg"}), "vector segment emitted");
    check(wire::emit_path_escape(v, 0x33, bytes_of("xy")), "vector escape emitted");
    check(wire::emit_path_label(v, {.index = 5, .generation = 2}), "vector label emitted");
    tr::detail::append_le(v, std::uint16_t{0xBEEF});

    check(wire::emit_header(b, type_t::VALUE, opt_t{}, 3) &&
              wire::emit_tlv(b, type_t::POINT, opt_t{.pl = true}, bytes_of("abc")) &&
              wire::emit_tlv(b, type_t::VALUE, opt_t{.ts = true}, big) &&
              wire::emit_name(b, "sensor") && wire::emit_name(b, bytes_of("raw")) &&
              wire::emit_value_le(b, std::uint32_t{0xA1B2C3D4}, 3) &&
              wire::emit_trailer_ts(b, false, 1'700'000'000'000'000'000) &&
              wire::emit_trailer_ts(b, true, -42) && wire::emit_path_ref(b, refs) &&
              wire::emit_path_segment(b, std::string_view{"seg"}) &&
              wire::emit_path_escape(b, 0x33, bytes_of("xy")) &&
              wire::emit_path_label(b, {.index = 5, .generation = 2}) &&
              tr::detail::append_le(b, std::uint16_t{0xBEEF}),
          "every bytes_t emitter accepted");
    check(same(b, v), "…and wrote exactly the vector forms' bytes");

    // The refusal arms the vector forms share: nothing is appended.
    mem::bytes_t r(mem::heap_source());
    check(!wire::emit_path_label(r, {}) && r.empty(), "the reserved zero label is refused");
    check(!wire::emit_path_segment(r, std::string_view{}) && r.empty(),
          "an empty segment is refused");
    check(!wire::emit_path_ref(r, refs, type_t::VALUE) && r.empty(),
          "a non-bound-path type is refused");
}

void test_path_elements_match() {
    std::printf("path_element splice and element forms match:\n");
    std::vector<std::byte> body;
    for (const std::string_view s : {"net", "tcp0", "peer", "x"})
        check(wire::emit_path_segment(body, s), "body segment");
    const tr::wire::path_label_t label{.index = 3, .generation = 9};
    std::vector<std::byte> v;
    mem::bytes_t b(mem::heap_source());
    check(wire::emit_path_labelled(v, body, 1, 2, label), "vector splice accepted");
    check(wire::emit_path_labelled(b, body, 1, 2, label), "bytes_t splice accepted");
    check(same(b, v), "…and they spell the same bytes");
    mem::bytes_t r(mem::heap_source());
    check(!wire::emit_path_labelled(r, body, 3, 2, label) && r.empty(),
          "an out-of-range run is refused, appending nothing");

    std::vector<std::byte> ve;
    mem::bytes_t be(mem::heap_source());
    tr::wire::path_element_cursor_t cur(v);
    std::size_t n = 0;
    while (const auto el = cur.next()) {
        check(wire::emit_path_element(ve, *el) && wire::emit_path_element(be, *el),
              "element re-spelled");
        ++n;
    }
    check(n == 3 && same(be, ve) && ve == v, "every element round-trips through both forms");
}

void test_batch_match() {
    std::printf("batch emitters match:\n");
    const std::array<std::byte, 4> s0{std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
                                      std::byte{0x00}};
    const std::array<std::byte, 5> s1{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
                                      std::byte{0x00}, std::byte{0x7F}};
    const std::array<std::span<const std::byte>, 2> samples{s0, s1};
    const std::array<std::int32_t, 2> offs{0, 250};
    std::vector<std::byte> v;
    mem::bytes_t b(mem::heap_source());
    wire::emit_batch(v, 1'000, samples, offs);
    wire::emit_batch_time(v, 77);
    wire::emit_batch_offsets(v, offs);
    check(wire::emit_batch(b, 1'000, samples, offs) && wire::emit_batch_time(b, 77) &&
              wire::emit_batch_offsets(b, offs),
          "bytes_t batch emitters accepted");
    check(same(b, v), "…and wrote the vector forms' bytes");
    mem::bytes_t r(mem::null_source());
    check(!wire::emit_batch(r, 1'000, samples, offs) && r.empty(),
          "a refusing source appends nothing");
}

void test_encode_match() {
    std::printf("encode(tlv, bytes_t&) and the path_key view:\n");
    const std::array<std::byte, 3> leaf{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::vector<std::byte> big(0x10001, std::byte{0x11});
    wire::tlv_t stamped{
        .type = type_t::VALUE,
        .opt = opt_t{.ts = true, .cr = true},
        .payload = leaf,
        .trailer = wire::trailer_t{.ts = wire::timestamp_t{.value = 12345}, .crc = {}}};
    wire::tlv_t crc16{.type = type_t::NAME, .opt = opt_t{.cr = true, .cw = true}, .payload = leaf};
    wire::tlv_t wide{.type = type_t::VALUE, .payload = big};
    wire::tlv_t root{.type = type_t::POINT, .opt = opt_t{.pl = true}};
    root.children = {stamped, crc16, wide};
    const std::vector<std::byte> v = wire::encode(root);
    check(!v.empty(), "the vector encode accepted the tree");
    mem::bytes_t b(mem::heap_source());
    check(wire::encode(root, b) && same(b, v), "the bytes_t encode wrote the same frame");

    // A refused descendant refuses the whole tree, and nothing is appended.
    wire::tlv_t bad_ts{.type = type_t::VALUE, .opt = opt_t{.ts = true}, .payload = leaf};
    wire::tlv_t parent{.type = type_t::POINT, .opt = opt_t{.pl = true}};
    parent.children = {bad_ts};
    mem::bytes_t r(mem::heap_source());
    check(wire::encode(parent).empty() && !wire::encode(parent, r) && r.empty(),
          "a stamp with no value refuses both forms, appending nothing");
    mem::bytes_t none(mem::null_source());
    check(!wire::encode(root, none) && none.empty(), "a refusing source appends nothing");

    std::vector<std::byte> key;
    check(wire::emit_path_segment(key, std::string_view{"a"}) &&
              wire::emit_path_segment(key, std::string_view{"bc"}),
          "key built");
    std::vector<std::byte> path_frame;
    wire::emit_tlv(path_frame, type_t::PATH, opt_t{}, key);
    const auto node = wire::tlv_node_t::over(std::span<const std::byte>(path_frame));
    check(node.has_value(), "PATH frame validates");
    const auto viewed = wire::path_key(*node);
    check(viewed && std::ranges::equal(*viewed, key) &&
              viewed->data() >= path_frame.data() &&
              viewed->data() + viewed->size() <= path_frame.data() + path_frame.size(),
          "path_key is the key, as a view of the frame's own PATH body");
}

void test_iovec_span() {
    std::printf("to_iovec into caller storage:\n");
    const std::array<std::byte, 2> a{std::byte{1}, std::byte{2}};
    const std::array<std::byte, 3> c{std::byte{3}, std::byte{4}, std::byte{5}};
    tr::view::rope_t rope;
    for (const auto& part : {std::span<const std::byte>(a), std::span<const std::byte>(c),
                             std::span<const std::byte>(a)}) {
        const auto v = tr::view::over_bytes(part);
        check(v.has_value(), "segment");
        rope.append(*v);
    }
    std::array<std::span<const std::byte>, 4> iov{};
    check(rope.to_iovec(iov) == 3, "the whole rope fits: the link count comes back");
    const auto ref = rope.to_iovec();
    bool eq = ref.size() == 3;
    for (std::size_t i = 0; eq && i < 3; ++i)
        eq = iov[i].data() == ref[i].data() && iov[i].size() == ref[i].size();
    check(eq, "…and the spans are the vector form's, zero-copy");
    std::array<std::span<const std::byte>, 1> short_iov{};
    check(rope.to_iovec(short_iov) == 3 && short_iov[0].data() == ref[0].data(),
          "a short table fills what fits and still reports the full count");
}

void test_acl_and_spec() {
    std::printf("encode_acl / parse_acl / conn_spec into core containers:\n");
    using tr::graph::ace_t;
    std::vector<ace_t> aces(2);
    aces[0].subject.assign(bytes_of("peer-a").begin(), bytes_of("peer-a").end());
    aces[0].access_mask = 0x41;
    aces[0].flags = tr::graph::kAceInherit;
    aces[1].subject.assign(bytes_of("EVERYONE@").begin(), bytes_of("EVERYONE@").end());
    aces[1].access_mask = 0x01;
    aces[1].expires_ns = 99;
    const std::vector<std::byte> v = tr::graph::encode_acl(aces);
    mem::bytes_t b(mem::heap_source());
    check(tr::graph::encode_acl(aces, b) && same(b, v), "encode_acl into bytes_t == vector");
    mem::bytes_t none(mem::null_source());
    check(!tr::graph::encode_acl(aces, none) && none.empty(), "a refusing source: nothing");

    const auto node = wire::tlv_node_t::over(std::span<const std::byte>(v));
    check(node.has_value(), "the encoded ACL validates");
    const auto parsed = tr::graph::parse_acl(*node);
    mem::block_array_t<ace_t> table(mem::heap_source());
    check(parsed && tr::graph::parse_acl(*node, table).has_value() && table.size() == 2,
          "parse_acl into a core array accepted both ACEs");
    bool eq = parsed && parsed->size() == table.size();
    for (std::size_t i = 0; eq && i < table.size(); ++i)
        eq = (*parsed)[i].subject == table[i].subject &&
             (*parsed)[i].access_mask == table[i].access_mask &&
             (*parsed)[i].expires_ns == table[i].expires_ns && (*parsed)[i].flags == table[i].flags;
    check(eq, "…and holds what the vector form parses");
    mem::block_array_t<ace_t> refused(mem::null_source());
    const auto r = tr::graph::parse_acl(*node, refused);
    check(!r && r.error() == tr::graph::status_t::BACKPRESSURE && refused.empty(),
          "a refusing source answers BACKPRESSURE and holds no ACE");

    tr::net::conn_spec_t spec("link0");
    spec.port(7000).kind("tcp");
    mem::bytes_t sb(mem::heap_source());
    check(spec.bytes(sb) && same(sb, spec.bytes()), "conn_spec_t::bytes(bytes_t&) == bytes()");
    tr::net::conn_spec_t bare("bare");
    mem::bytes_t bb(mem::heap_source());
    check(bare.bytes(bb) && same(bb, bare.bytes()), "…and so does the config-less SPEC");
}

void test_graph_arrays() {
    std::printf("graph read_subscribers / drain_unflushed into core arrays:\n");
    using tr::graph::role_t;
    tr::graph::graph_t g;
    const auto src = g.register_vertex(tr::graph::path_t("/s"), role_t::STORED_VALUE);
    for (const std::string_view link : {"link-a", "link-b"})
        check(g.subscribe_wire(src, tr::testing::make_value({0x04, 0x40, 0x00, 0x00}),
                               tr::testing::make_value({0x06, 0x00, 0x00, 0x00}), link)
                  .has_value(),
              "a wire edge (the link is passed as a view)");
    const auto legacy = g.read_subscribers(src);
    mem::block_array_t<tr::view::view_t> subs(mem::heap_source());
    const auto n = g.read_subscribers(src, subs);
    check(legacy && legacy->size() == 2 && n && *n == 2 && subs.size() == 2,
          "read_subscribers into a core array answers the vector form's views");
    check(n && legacy && subs[0].bytes().data() == (*legacy)[0].bytes().data(),
          "…the same stored views, cloned not copied");
    mem::block_array_t<tr::view::view_t> refused(mem::null_source());
    const auto rn = g.read_subscribers(src, refused);
    check(!rn && rn.error() == tr::graph::status_t::BACKPRESSURE,
          "a refusing source answers BACKPRESSURE");

    const auto ring = g.register_vertex(tr::graph::path_t("/r"), role_t::STREAM);
    check(g.set_policy(ring, {.retention = tr::graph::retention_t::N, .depth = 4}).has_value(),
          "a four-deep ring");
    for (std::uint8_t i = 0; i < 3; ++i)
        check(g.assign(ring, tr::testing::make_value({i})).has_value(),
              "stream append (assign: `write` would drain on its own delivery)");
    mem::block_array_t<tr::graph::value_ref_t> none(mem::null_source());
    const auto zero = g.drain_unflushed(ring, none);
    check(zero && *zero == 0, "a refused snapshot drains nothing…");
    mem::block_array_t<tr::graph::value_ref_t> out(mem::heap_source());
    const auto got = g.drain_unflushed(ring, out);
    check(got && *got == 3 && out.size() == 3, "…and leaves all three owed to the next drain");
    const auto again = g.drain_unflushed(ring, out);
    check(again && *again == 0, "a drain advances the cursor");
}

/** @brief The caller-storage resolver: the caller IS the subject; "ghost" is unnameable. */
std::expected<void, tr::wire::err_t> lookup_caller(void*, std::string_view caller,
                                                   mem::bytes_t& out) {
    if (caller == "ghost") return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    if (!out.append(bytes_of(caller).data(), caller.size()))
        return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    return {};
}

/** @brief The returning resolver, for the adapter arm. */
std::expected<tr::graph::subject_token_t, tr::wire::err_t> resolve_caller(void*,
                                                                          std::string_view caller) {
    if (caller == "ghost") return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    return tr::graph::subject_token_t(bytes_of(caller).begin(), bytes_of(caller).end());
}

/** @brief Gate a read of `/x` for each caller and report allow/deny as a 3-char string. */
[[nodiscard]] std::array<bool, 3> gate(tr::graph::graph_t& g, tr::graph::vertex_handle_t v,
                                       std::string_view long_name) {
    return {g.read(v, "peer-a").has_value(), g.read(v, "peer-b").has_value(),
            g.read(v, long_name).has_value()};
}

void test_subject_lookup() {
    std::printf("subject_lookup writes the token into the gate's frame:\n");
    using tr::graph::acl_right_t;
    using tr::graph::role_t;
    // A subject longer than the gate's 64 B stack frame takes the spill arm.
    const std::string_view long_name =
        "a-very-long-subject-name-that-does-not-fit-the-gate-stack-frame-of-64-bytes";
    std::vector<tr::graph::ace_t> aces(2);
    aces[0].subject.assign(bytes_of("peer-a").begin(), bytes_of("peer-a").end());
    aces[0].access_mask = static_cast<std::uint32_t>(acl_right_t::READ);
    aces[1].subject.assign(bytes_of(long_name).begin(), bytes_of(long_name).end());
    aces[1].access_mask = static_cast<std::uint32_t>(acl_right_t::READ);
    const std::vector<std::byte> acl = tr::graph::encode_acl(aces);

    for (const bool caller_storage : {true, false}) {
        tr::graph::graph_t g;
        auto hooks = g.hooks();
        if (caller_storage) {
            hooks.subject_lookup = {lookup_caller, nullptr};
        } else {
            hooks.subject_resolver = {resolve_caller, nullptr};
        }
        g.set_hooks(hooks);
        const auto v = g.register_vertex(tr::graph::path_t("/x"), role_t::STORED_VALUE);
        check(g.write(v, tr::testing::make_value({7})).has_value(), "value");
        check(g.write(tr::graph::path_t("/x:acl"), tr::testing::make_value(acl)).has_value(),
              "a closing :acl");
        const auto seen = gate(g, v, long_name);
        check(seen[0] && !seen[1] && seen[2],
              caller_storage ? "subject_lookup: peer-a and the spilled long subject read, "
                               "peer-b does not"
                             : "the returning resolver gates identically through the adapter");
        check(!g.read(v, "ghost").has_value(), "an unnameable caller is denied");
        const auto back = g.hooks();
        check(
            caller_storage
                ? back.subject_lookup.fn == &lookup_caller && back.subject_resolver.fn == nullptr
                : back.subject_resolver.fn == &resolve_caller && back.subject_lookup.fn == nullptr,
            "hooks() hands back exactly what was installed, never the adapter");
    }
}

}  // namespace

int main() {
    test_emitters_match();
    test_path_elements_match();
    test_batch_match();
    test_encode_match();
    test_iovec_span();
    test_acl_and_spec();
    test_graph_arrays();
    test_subject_lookup();
    return tr::testing::summary("public_api_views");
}
