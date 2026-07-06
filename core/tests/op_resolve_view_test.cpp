/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0053 §7 (3c-ii) — the DIFFERENTIAL ORACLE for the two terminus-resolver
 * instantiations. The SAME logical request FWD is resolved twice against two
 * identically-seeded graphs: once through the span-tier `arena_node` reader (the
 * arena decode of the flattened frame — `op_resolver_t::resolve(tlv_arena_t)`),
 * and once through the owning rope-tier `view_node` reader (`resolve(tlv_view_t)`
 * over the frame as a scatter-gather rope). The flattened `FWD{REPLY}` bytes MUST
 * be byte-identical for every rope split — that is the proof the ONE templated
 * `resolve_node` walk behaves the same whether it reads random-access arena spans
 * or a forward-only lazy view.
 *
 * Splits are adversarial: single-link (zero-copy adopt), a 2-link cut at every
 * interior boundary (forcing the per-node interim flatten), and one-link-per-byte
 * (max fragmentation — every header stitch and body materialize exercised). All
 * frames are built WITHOUT CRC trailers, so this slice sidesteps the per-TLV
 * verify-at-access divergence (ADR-0053 §4) that 3c-iii wires in.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// --- wire builders (canonical, trailer-less bytes via the production helpers) --
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> p;
    for (std::uint8_t b : bytes) p.push_back(std::byte{b});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}
std::vector<std::byte> b_subscriber(std::initializer_list<std::string_view> target) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path(target));
    return out;
}
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body = b_name("subscribers");
    append(body, b_value({0x01}));  // index_mode=ELEMENT, no index => "[]"
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& selector = {},
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    append(body, b_value({static_cast<std::uint8_t>(op)}));
    append(body, dst);
    if (!selector.empty()) append(body, selector);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

// A view_t over a fresh owned heap segment holding `bytes`.
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

// Build a rope over `bytes` split at the given cut points (each cut a link
// boundary) — every link its own heap segment, a genuine scatter-gather frame.
tr::view::rope_t rope_split(std::span<const std::byte> bytes, std::span<const std::size_t> cuts) {
    tr::view::rope_t r;
    std::size_t prev = 0;
    const auto add = [&](std::size_t from, std::size_t to) {
        if (to > from) r.append(make_value(bytes.subspan(from, to - from)));
    };
    for (const std::size_t c : cuts) {
        const std::size_t cut = c > bytes.size() ? bytes.size() : c;
        add(prev, cut);
        prev = cut;
    }
    add(prev, bytes.size());
    return r;
}

using seed_fn = std::function<void(graph_t&)>;

// Resolve `frame` through the SPAN tier: arena-decode the contiguous bytes and
// resolve. Returns the flattened reply bytes ("" marks a hard error side).
std::vector<std::byte> resolve_arena_flat(const seed_fn& seed, std::span<const std::byte> frame,
                                          std::string_view inbound_link) {
    graph_t g;
    seed(g);
    op_resolver_t r(g);
    const auto arena = tr::wire::decode_into(frame, *std::pmr::get_default_resource());
    if (!arena) return {};
    auto reply = r.resolve(*arena, inbound_link);
    if (!reply) return {};
    const tr::view::view_t flat = reply->flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

// Resolve the SAME frame through the OWNING rope tier: adopt the rope as a lazy
// `tlv_view_t` and resolve. Returns the flattened reply bytes.
std::vector<std::byte> resolve_view_flat(const seed_fn& seed, tr::view::rope_t rope,
                                         std::string_view inbound_link) {
    graph_t g;
    seed(g);
    op_resolver_t r(g);
    const auto view = tr::wire::tlv_view_t::over(std::move(rope));
    if (!view) return {};
    auto reply = r.resolve(*view, inbound_link);
    if (!reply) return {};
    const tr::view::view_t flat = reply->flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

// The core assertion: for `frame`, the view-tier reply equals the arena-tier
// reply (the oracle) for a single-link rope, every 2-link interior split, and the
// one-link-per-byte rope. Also asserts the oracle itself is non-empty (a real
// reply, not two matching error sides) unless `expect_reply` is false.
void differential(std::string_view name, const seed_fn& seed, const std::vector<std::byte>& frame,
                  std::string_view inbound_link = {}) {
    std::printf("%.*s:\n", static_cast<int>(name.size()), name.data());
    const std::vector<std::byte> oracle = resolve_arena_flat(seed, frame, inbound_link);
    check(!oracle.empty(), "arena-tier oracle produced a (non-empty) reply");

    // Single-link rope (zero-copy adopt path).
    {
        const auto got = resolve_view_flat(seed, rope_split(frame, {}), inbound_link);
        check(got == oracle, "single-link rope resolves byte-identically to the arena oracle");
    }
    // Every 2-link interior split (each forces the per-node interim flatten).
    int mismatches = 0;
    int checked = 0;
    for (std::size_t cut = 1; cut < frame.size(); ++cut) {
        const std::array<std::size_t, 1> cuts{cut};
        const auto got = resolve_view_flat(seed, rope_split(frame, cuts), inbound_link);
        ++checked;
        if (got != oracle) ++mismatches;
    }
    check(checked > 0, "swept every interior split boundary");
    check(mismatches == 0, "every 2-link split resolves byte-identically to the arena oracle");
    // Max fragmentation: one link per byte.
    {
        std::vector<std::size_t> every_byte;
        for (std::size_t i = 1; i < frame.size(); ++i) every_byte.push_back(i);
        const auto got = resolve_view_flat(seed, rope_split(frame, every_byte), inbound_link);
        check(got == oracle,
              "one-link-per-byte rope resolves byte-identically (max fragmentation)");
    }
}

// Seed helpers -----------------------------------------------------------------
void seed_temp_value(graph_t& g) {
    const auto path = path_t::parse("/sensor/temp");
    tr::graph::vertex_t* v = *g.register_vertex(*path, role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0xD2, 0x04, 0x00, 0x00})));  // VALUE u32=1234
}
void seed_temp_empty(graph_t& g) {
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
}
// Register /sensor/temp and bind two REMOTE subscribers (via the resolver itself,
// so both graphs reach byte-identical slot state) for the :subscribers[] read.
void seed_temp_with_remote_subs(graph_t& g) {
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    op_resolver_t r(g);
    const auto field = b_field_subscribers_append();
    for (const char* tgt : {"sub-a", "sub-b"}) {
        std::initializer_list<std::string_view> t{tgt};
        const auto wfwd = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                                field, b_subscriber(t));
        const auto arena = tr::wire::decode_into(wfwd, *std::pmr::get_default_resource());
        (void)r.resolve(*arena, "cli");  // inbound_link set => REMOTE subscriber binding
    }
}

}  // namespace

int main() {
    std::printf("ADR-0053 §7 differential oracle: view-tier resolver == arena-tier resolver\n\n");

    // READ a stored value — reply carries the value payload.
    differential("READ /sensor/temp (RESULT with payload)", seed_temp_value,
                 b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"reply-ep"})));

    // WRITE a value — reply RESULT (empty payload); also assert both tiers left the
    // SAME stored value behind (the walk applied the op identically, not just replied).
    {
        const auto wframe = b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                                  {}, b_value({0x2A}));
        differential("WRITE /sensor/temp (RESULT, LKV updated)", seed_temp_empty, wframe);

        graph_t ga;
        seed_temp_empty(ga);
        op_resolver_t ra(ga);
        const auto arena = tr::wire::decode_into(wframe, *std::pmr::get_default_resource());
        (void)ra.resolve(*arena);
        graph_t gv;
        seed_temp_empty(gv);
        op_resolver_t rv(gv);
        const auto view =
            tr::wire::tlv_view_t::over(rope_split(wframe, std::array<std::size_t, 1>{5}));
        (void)rv.resolve(*view);
        const auto sa = ga.read(ga.find(path_t::parse("/sensor/temp")->key()));
        const auto sv = gv.read(gv.find(path_t::parse("/sensor/temp")->key()));
        check(sa.has_value() && sv.has_value() &&
                  sa->flatten().bytes().size() == sv->flatten().bytes().size() &&
                  std::memcmp(sa->flatten().bytes().data(), sv->flatten().bytes().data(),
                              sa->flatten().bytes().size()) == 0,
              "both tiers stored byte-identical LKV after the WRITE");
    }

    // ADR-0053 ⑤ / ADR-0042 §3 — the rope-tier referenced store: a view-delivered
    // MULTI-LINK payload on a vertex that opted in (store_ref_min_bytes > 0) is PINNED
    // as a subrope of the delivery (its segments kept, ZERO copy), yet byte-identical
    // to the copy a default vertex makes. The arena tier proves the contiguous-frame
    // twin in op_resolve_test's store_ref_threshold; here the payload spans many links.
    {
        std::printf("rope-tier pinned store (store_ref_min_bytes, multi-link payload):\n");
        graph_t g;
        tr::graph::vertex_t* v =
            *g.register_vertex(*path_t::parse("/sensor/blob"), role_t::STORED_VALUE);
        // Opt in via the :settings field-write (parses the u32), matching the arena test.
        (void)g.write(*path_t::parse("/sensor/blob:settings.store_ref_min_bytes"),
                      make_value(b_value({0x08, 0x00, 0x00, 0x00})));

        std::vector<std::byte> big(32);
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>(i);
        std::vector<std::byte> big_tlv;  // a 36-byte trailer-less VALUE TLV
        tr::wire::emit_tlv(big_tlv, type_t::VALUE, opt_t{}, big);
        const auto wframe =
            b_fwd(fwd_op_t::WRITE, b_path({"sensor", "blob"}), b_path({"reply-ep"}), {}, big_tlv);

        // One link per byte: the payload TLV is guaranteed to span many links.
        std::vector<std::size_t> every_byte;
        for (std::size_t i = 1; i < wframe.size(); ++i) every_byte.push_back(i);
        op_resolver_t r(g);
        const auto view = tr::wire::tlv_view_t::over(rope_split(wframe, every_byte));
        check(view.has_value(), "fragmented WRITE frame adopts as a lazy view");
        const auto reply = r.resolve(*view);
        check(reply.has_value(), "rope-tier WRITE over threshold produced a reply");

        const auto rd = g.read(v);
        check(rd.has_value() && rd->link_count() > 1,
              "multi-link payload PINNED as a subrope (segments kept, not copied to one buffer)");
        check(rd.has_value() && rd->flatten().bytes().size() == big_tlv.size() &&
                  std::memcmp(rd->flatten().bytes().data(), big_tlv.data(), big_tlv.size()) == 0,
              "pinned store reads back byte-identical to the written VALUE TLV");
    }

    // Remote subscribe — a :subscribers[] APPEND over a link (inbound_link set)
    // carrying a SUBSCRIBER: a fan-out edge bound identically by both tiers.
    differential("WRITE :subscribers[] remote subscribe (RESULT)", seed_temp_empty,
                 b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                       b_field_subscribers_append(), b_subscriber({"sub-x"})),
                 "cli");

    // READ :subscribers[] — the POINT-wrapped rope of slot views.
    differential("READ :subscribers[] (POINT wrapper of slot views)", seed_temp_with_remote_subs,
                 b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                       b_field_subscribers_append()));

    // NOT_FOUND — an unregistered dst replies kind=ERROR; the error reply must be
    // byte-identical across tiers too (same swapped route + STATUS payload).
    differential("READ /nope/missing (ERROR NOT_FOUND)", seed_temp_empty,
                 b_fwd(fwd_op_t::READ, b_path({"nope", "missing"}), b_path({"reply-ep"})));

    // Write-create (RFC-0005) — a remote DATA write to a fresh path creates it.
    differential("WRITE /fresh/leaf write-create (RESULT)", seed_temp_empty,
                 b_fwd(fwd_op_t::WRITE, b_path({"fresh", "leaf"}), b_path({"reply-ep"}), {},
                       b_value({0x5A})),
                 "cli");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
