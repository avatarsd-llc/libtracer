/**
 * @file
 * @brief #1503 step 5 / RFC-0010 Amendment 1 — the node-scoped seam census: `read
 *        <vertex>:stats.<class>.<seam>`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The amendment's five normative claims, one test each:
 *   - §D.3 ONE read = ONE seam = the whole counter block in ONE `SETTINGS` TLV, sampled in
 *     one call — the only shape under which `core/STYLE.md` §Introspection's
 *     snapshot-coherence clause can hold, and the reason six per-counter fields were
 *     refused;
 *   - §D.1 node-scoped in the `:identity` mould — the field takes no vertex, every vertex
 *     answers identically, and the content describes the NODE;
 *   - §D.5 the gating is INVERTED against `:identity`: the VALUE resolves BELOW the READ
 *     gate, so a denied caller gets `PERMISSION_DENIED` (and thereby learns the seam
 *     exists — intended), while `:identity` on the same vertex stays pre-auth;
 *   - §D.2 NAME validity resolves ABOVE the gate, so every unrecognised `:stats` spelling
 *     answers `SCHEMA_NOT_FOUND` caller-INDEPENDENTLY — no spelling has two answers split
 *     by who asked (the RFC-0010 erratum's protocol-owned row);
 *   - §D.6 readable, never writable, never awaitable. The write half is here; the AWAIT
 *     half is `op_resolve_test.cpp`'s `test_await_field_selector_is_enotty`, which already
 *     refuses EVERY field-tailed AWAIT unconditionally (#585) and now names `:stats`
 *     explicitly — the amendment CITES that shipped rule rather than minting one.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief A delivery-counting subscriber — the control that proves nothing was published. */
void count_cb(void* ctx, const tr::view::rope_t& /*value*/) { ++*static_cast<int*>(ctx); }

/** @brief The test resolver (ADR-0018): the caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

std::string hex(std::span<const std::byte> b) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (std::byte x : b) {
        out.push_back(d[std::to_integer<unsigned>(x) >> 4]);
        out.push_back(d[std::to_integer<unsigned>(x) & 0xF]);
    }
    return out;
}

/** @brief Read `path:field` as @p caller — the handle+field overload the FWD resolver uses. */
tr::graph::result_t<tr::view::rope_t> read_as(graph_t& g, const char* path,
                                              std::string_view caller) {
    const auto p = path_t::parse(path);
    if (!p) return std::unexpected(status_t::INVALID_PATH);
    const auto v = g.find(p->key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return g.read(*v, p->field(), caller);
}

/** @brief The flattened wire bytes of a field read (empty on error). */
std::vector<std::byte> read_bytes(graph_t& g, const char* path, std::string_view caller) {
    const auto r = read_as(g, path, caller);
    if (!r) return {};
    const tr::view::view_t flat = r->flatten();
    const auto span = flat.bytes();
    return std::vector<std::byte>(span.begin(), span.end());
}

/** @brief The u64 value of the member named @p noun in a decoded census block, or `-1`. */
std::uint64_t counter(const tr::wire::tlv_t& block, std::string_view noun) {
    for (std::size_t i = 0; i + 1 < block.children.size(); i += 2) {
        if (block.children[i].type != type_t::NAME) continue;
        if (tr::detail::as_string_view(block.children[i].payload) != noun) continue;
        if (block.children[i + 1].type != type_t::VALUE) break;
        if (block.children[i + 1].payload.size() != 8) break;
        return tr::detail::load_le<std::uint64_t>(block.children[i + 1].payload);
    }
    return static_cast<std::uint64_t>(-1);
}

/**
 * @brief §D.3 — one seam, one block, one TLV: the shape, the vocabulary, and the values.
 *
 * The block is pinned BYTE-EXACT for the all-zero graph-door seam, because those are the
 * bytes the `settings/stats-seam-block` conformance vector holds; the mem seam is asserted
 * against a source whose numbers the test itself provokes, so a block that merely LOOKED
 * right (all zeros, wrong seam, stale sample) cannot pass.
 */
void test_one_seam_is_one_block() {
    std::printf("RFC-0010 Am.1 §D.3: one READ = one seam = the whole block in ONE TLV:\n");

    // The graph door, on a fresh node: four counters, all zero. These bytes are the vector.
    {
        graph_t g;
        (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
        const auto bytes = read_bytes(g, "/sensor/temp:stats.graph.delivery", {});
        const auto dec = tr::wire::decode(bytes);  // `bytes` outlives it — children are spans
        check(dec && dec->type == type_t::SETTINGS && dec->opt.pl,
              "the answer is ONE structured SETTINGS TLV");
        check(dec && dec->children.size() == 8, "four NAME/VALUE pairs — the whole door block");
        check(counter(*dec, "no_target") == 0 && counter(*dec, "denied") == 0 &&
                  counter(*dec, "out_of_memory") == 0 && counter(*dec, "fan_out_truncated") == 0,
              "the four delivery_drops_t nouns are present and zero on a fresh node");
        const std::string expect =
            "0b406d00"                                    // SETTINGS PL=1, len=109
            "020009006e6f5f746172676574"                  // NAME "no_target"
            "010008000000000000000000"                    // VALUE u64 = 0
            "0200060064656e696564"                        // NAME "denied"
            "010008000000000000000000"                    // VALUE u64 = 0
            "02000d006f75745f6f665f6d656d6f7279"          // NAME "out_of_memory"
            "010008000000000000000000"                    // VALUE u64 = 0
            "0200110066616e5f6f75745f7472756e6361746564"  // NAME "fan_out_truncated"
            "010008000000000000000000";                   // VALUE u64 = 0
        check(hex(bytes) == expect, "the block is byte-exact (the stats-seam-block vector)");
        if (hex(bytes) != expect) std::printf("  actual: %s\n", hex(bytes).c_str());
    }

    // A mem seam, with numbers this test provokes: a pool too small for the request.
    {
        std::array<std::byte, 4096> slab{};
        std::array<tr::mem::size_class_t, 8> classes{};
        tr::mem::pool_source_t pool(slab, classes);
        graph_t g(std::pmr::get_default_resource(), &tr::mem::heap_backend(), &pool);
        (void)g.register_vertex(path_t("/n"), role_t::STORED_VALUE);

        // The bytes must OUTLIVE the decode: a `tlv_t`'s children are spans INTO them.
        const auto before_bytes = read_bytes(g, "/n:stats.mem.control", {});
        const auto before = tr::wire::decode(before_bytes);
        check(before && counter(*before, "capacity") == slab.size(),
              "capacity is the INJECTED slab, not a compile-time constant");
        check(before && counter(*before, "refused") == 0, "…and nothing is refused yet");

        void* p = pool.try_alloc(1u << 20, alignof(std::max_align_t));
        check(p == nullptr, "a 1 MiB request against a 4 KiB slab is refused BY VALUE");

        const auto after_bytes = read_bytes(g, "/n:stats.mem.control", {});
        const auto after = tr::wire::decode(after_bytes);
        check(after && counter(*after, "refused") == 1,
              "the SAME field now reports the refusal — the census is live, not a snapshot "
              "frozen at construction");
        check(after && counter(*after, "largest_refused") == (1u << 20),
              "…and largest_refused is in the SAME block: one read, one coherent sample");
        check(after && after->children.size() == 10,
              "five nouns for a mem seam (capacity/in_use/peak/refused/largest_refused)");

        // The ring seam is a DIFFERENT seam and says so — per-seam, never aggregated
        // (ADR-0079). This graph left `ring` at the default heap source.
        const auto ring_bytes = read_bytes(g, "/n:stats.mem.ring", {});
        const auto ring = tr::wire::decode(ring_bytes);
        check(ring && counter(*ring, "refused") == 0,
              ":stats.mem.ring is the OTHER seam — the control seam's refusal is not its");
    }
}

/** @brief §D.1 — node-scoped: the field takes no vertex; every vertex answers identically. */
void test_node_scoped() {
    std::printf("RFC-0010 Am.1 §D.1: node-scoped — every vertex answers identically:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/actuator/relay/0"), role_t::STORED_VALUE);

    const auto a = read_bytes(g, "/sensor/temp:stats.graph.delivery", {});
    const auto b = read_bytes(g, "/actuator/relay/0:stats.graph.delivery", {});
    check(!a.empty() && a == b, "two unrelated vertices return BYTE-IDENTICAL census blocks");

    (void)g.register_vertex(path_t("/late/arrival"), role_t::STORED_VALUE);
    check(read_bytes(g, "/late/arrival:stats.graph.delivery", {}) == a,
          "a vertex created afterwards answers identically (nothing is per-vertex)");
}

/**
 * @brief §D.5 — the gating is INVERTED against `:identity`: `:stats` sits BELOW the READ gate.
 *
 * The load-bearing test of the ruling. A memory census is not first-contact material, so the
 * narrow pre-auth exemption `:identity` holds is deliberately NOT extended: a denied caller
 * is answered `PERMISSION_DENIED`, which discloses that the seam exists — intended, and no
 * more than the published `settings` / `children` namespaces already disclose.
 */
void test_value_is_read_gated() {
    std::printf("RFC-0010 Am.1 §D.5: the VALUE is READ-gated (inverted vs :identity):\n");
    graph_t g;
    g.configure_subject_resolver(caller_is_subject, nullptr);
    const auto v = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    (void)g.write(v, make_value({std::uint8_t{7}}));
    check(g.set_identity(0x01, std::array<std::byte, 32>{}).has_value(), "identity installed");

    const std::vector<ace_t> grant{
        ace_t{.type = tr::graph::ace_type_t::ALLOW,
              .flags = 0,
              .subject = as_bytes("alice"),
              .access_mask = static_cast<std::uint32_t>(acl_right_t::READ),
              .expires_ns = 0}};
    check(g.write(path_t("/dev:acl"), make_value(tr::graph::encode_acl(grant))).has_value(),
          "an ACL granting only alice is installed on /dev");

    const auto denied = read_as(g, "/dev:stats.graph.delivery", "mallory");
    check(!denied.has_value() && denied.error() == status_t::PERMISSION_DENIED,
          "a denied caller gets PERMISSION_DENIED — the census is NOT pre-auth");
    check(read_bytes(g, "/dev:stats.graph.delivery", "alice").size() > 8,
          "…and an admitted caller is served the block");

    // The control that makes the inversion legible: the ONE pre-auth field still is one.
    check(read_bytes(g, "/dev:identity", "mallory").size() == 60,
          "control: :identity is STILL served to mallory — the exemption names one field");
}

/**
 * @brief §D.2 — NAME validity resolves ABOVE the gate: every unrecognised `:stats` spelling
 *        answers SCHEMA_NOT_FOUND to EVERY caller.
 *
 * The anti-leak property the RFC-0010 erratum states for the protocol-owned namespace: no
 * spelling may have two answers split by who asked. Each spelling is driven at BOTH callers.
 */
void test_unknown_spellings_are_caller_independent() {
    std::printf("RFC-0010 Am.1 §D.2: unknown :stats spellings are caller-INDEPENDENT:\n");
    graph_t g;
    g.configure_subject_resolver(caller_is_subject, nullptr);
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    const std::vector<ace_t> grant{
        ace_t{.type = tr::graph::ace_type_t::ALLOW,
              .flags = 0,
              .subject = as_bytes("alice"),
              .access_mask = static_cast<std::uint32_t>(acl_right_t::READ),
              .expires_ns = 0}};
    check(g.write(path_t("/dev:acl"), make_value(tr::graph::encode_acl(grant))).has_value(),
          "an ACL granting only alice is installed on /dev");

    for (const char* spelling :
         {"/dev:stats", "/dev:stats.mem", "/dev:stats.graph", "/dev:stats.mem.nope",
          "/dev:stats.nope.control", "/dev:stats.mem.control.extra", "/dev:stats.mem.control[0]"}) {
        const auto allowed = read_as(g, spelling, "alice");
        const auto refused = read_as(g, spelling, "mallory");
        check(!allowed.has_value() && allowed.error() == status_t::SCHEMA_NOT_FOUND &&
                  !refused.has_value() && refused.error() == status_t::SCHEMA_NOT_FOUND,
              spelling);
    }
    // The control: the RECOGNISED spelling really does split — that is the gate working.
    check(
        read_bytes(g, "/dev:stats.mem.control", "alice").size() > 8 &&
            read_as(g, "/dev:stats.mem.control", "mallory").error() == status_t::PERMISSION_DENIED,
        "control: a RECOGNISED seam is served to alice and denied to mallory");
}

/** @brief §D.6 — readable, never writable: the census has no write door, caller-independently. */
void test_census_is_read_only() {
    std::printf("RFC-0010 Am.1 §D.6: :stats is read-only — a write is ENOTTY:\n");
    graph_t g;
    g.configure_subject_resolver(caller_is_subject, nullptr);
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    const auto p = path_t::parse("/dev:stats.mem.control");
    check(p.has_value(), "the spelling parses");
    const auto vh = g.find(p->key());
    check(vh.has_value(), "the vertex resolves");
    for (const char* caller : {"", "alice", "mallory"}) {
        const auto w = g.write(*vh, p->field(), make_value({std::uint8_t{1}}), caller);
        check(!w.has_value() && w.error() == status_t::SCHEMA_NOT_FOUND,
              "a WRITE of a census seam answers SCHEMA_NOT_FOUND, whoever asks");
    }
    // The control: the same door writes a real field fine.
    const auto sp = path_t::parse("/dev:acl");
    check(g.write(*vh, sp->field(), make_value(tr::graph::encode_acl({})), "").has_value(),
          "control: the write door still serves a writable field on the same vertex");
}

/**
 * @brief The counters are not writes: reading them advances nothing an `await` could see.
 *
 * The C++-side half of "readable, never awaitable" (§D.6). Counter updates deliberately do
 * NOT bump `write_seq_` — making them real writes would put a publish on the hot path — so
 * nothing can ever wake a wait on this surface, which is why the wire door refuses the
 * AWAIT by value instead of sleeping forever.
 */
void test_reading_the_census_is_not_a_write() {
    std::printf("RFC-0010 Am.1 §D.6: sampling the census publishes nothing:\n");
    graph_t g;
    const auto v = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    (void)g.write(v, make_value({std::uint8_t{9}}));

    int deliveries = 0;
    (void)g.subscribe(path_t("/s"), count_cb, &deliveries);
    const int base = deliveries;

    for (int i = 0; i < 8; ++i) {
        check(!read_bytes(g, "/s:stats.graph.delivery", {}).empty(), "the census reads");
        check(!read_bytes(g, "/s:stats.mem.control", {}).empty(), "…and so does the mem seam");
    }
    check(deliveries == base, "eight census reads delivered NOTHING to the subscriber");

    const auto val = g.read(path_t("/s"));
    check(val.has_value(), "and the vertex's published value still reads back");
}

}  // namespace

int main() {
    std::printf("#1503 step 5 / RFC-0010 Amendment 1 — the node-scoped `:stats` census\n\n");
    test_one_seam_is_one_block();
    test_node_scoped();
    test_value_is_read_gated();
    test_unknown_spellings_are_caller_independent();
    test_census_is_read_only();
    test_reading_the_census_is_not_a_write();
    std::printf("\nall :stats field checks passed\n");
    return 0;
}
