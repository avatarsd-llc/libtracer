/**
 * @file
 * @brief RFC-0024 — bound-path routing and minting, at the terminus.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The behavioural half of RFC-0024: what a `PATH_REF` MEANS, as opposed to what its bytes
 * are (`path_ref_test.cpp` owns the codec). Everything here is arranged around one property
 * — **a bound path never mis-routes**. Each way validation can fail gets a case that
 * actually reaches it and a case that proves the same frame WOULD have been delivered with a
 * sound element, so no guard here can be satisfied vacuously:
 *
 * - a stale generation drops, and the retired-then-revived address proves it drops rather
 *   than delivering into the new tenant;
 * - an out-of-range index drops, against an in-range one that lands;
 * - a residual that is not exactly one element drops, at both ends of the range;
 * - a mint is refused wherever the operation is, so a denied caller learns nothing;
 * - and the bound and canonical spellings of one operation produce byte-identical replies.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::graph::vertex_slot_t;
using tr::wire::opt_t;
using tr::wire::path_ref_element_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

// --- wire builders ----------------------------------------------------------

/** @brief A canonical `PATH` over @p segs (the mint key, and the fallback). */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief A `PATH_REF` over @p elements — the bound spelling of an address. */
std::vector<std::byte> b_path_ref(std::span<const path_ref_element_t> elements) {
    std::vector<std::byte> out;
    (void)tr::wire::emit_path_ref(out, elements);
    return out;
}

/** @brief The one-element bound spelling a terminus sees after every hop has consumed its own. */
std::vector<std::byte> b_path_ref_one(std::uint32_t index, std::uint32_t generation) {
    const path_ref_element_t e{.index = index, .generation = generation};
    return b_path_ref(std::span<const path_ref_element_t>(&e, 1));
}

std::vector<std::byte> b_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> p;
    for (std::uint8_t b : bytes) p.push_back(std::byte{b});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

using tr::testing::b_fwd_raw_op;

/** @brief Arena-decode + resolve — the terminus wiring `fwd_router_t` uses. */
tr::graph::result_t<tr::view::rope_t> resolve_bytes(op_resolver_t& resolver,
                                                    std::span<const std::byte> fwd,
                                                    std::string_view inbound_link = {}) {
    const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
    if (!arena) return std::unexpected(tr::graph::status_t::INVALID_PATH);
    return resolver.resolve(*arena, inbound_link);
}

/** @brief The flattened reply bytes (the consumer's one allowed copy). */
std::vector<std::byte> reply_bytes(const tr::view::rope_t& reply) {
    const tr::view::view_t flat = reply.flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

/** @brief What a decoded reply says: its kind, its error identity, and any minted binding. */
struct reply_facts_t {
    bool decoded = false;
    reply_kind_t kind = reply_kind_t::RESULT;
    std::uint16_t code = 0;
    bool has_mint = false;
    path_ref_element_t mint{};
    std::size_t child_count = 0;
};

reply_facts_t reply_facts(const tr::view::rope_t& reply) {
    const tr::view::view_t flat = reply.flatten();
    const auto dec = tr::wire::decode(flat.bytes());
    reply_facts_t out;
    if (!dec || dec->children.size() < 4) return out;
    out.decoded = true;
    out.child_count = dec->children.size();
    out.kind =
        static_cast<reply_kind_t>(tr::detail::load_le<std::uint8_t>(dec->children[3].payload));
    // The mint is a trailing child, PAST `kind` (index 3). Scanning the whole child list
    // would count the reply's own `src` — which is the request's `dst` echoed back, and is
    // itself a PATH_REF whenever the request was bound. That false positive is exactly the
    // shape a denied bound READ has, so a test asserting "no mint on a denial" would pass
    // while measuring the echo.
    for (std::size_t i = 4; i < dec->children.size(); ++i) {
        const tlv_t& c = dec->children[i];
        if (c.type != type_t::PATH_REF) continue;
        if (tr::wire::path_ref_element_count(c.payload.size()) != 1) continue;
        out.has_mint = true;
        out.mint = tr::wire::path_ref_element_at(c.payload, 0);
    }
    if (out.kind == reply_kind_t::ERROR && dec->children.size() >= 5) {
        const tlv_t& status = dec->children[4];
        if (status.type == type_t::STATUS && !status.children.empty() &&
            status.children[0].type == type_t::ERROR && !status.children[0].children.empty()) {
            out.code = tr::detail::load_le<std::uint16_t>(status.children[0].children[0].payload);
        }
    }
    return out;
}

/**
 * @brief The test resolver (ADR-0018): the caller context IS the subject token.
 *
 * The empty (local) context never reaches a resolver — `graph_t::acl_allows` settles it
 * as trusted before invoking one (#905) — so the error arm here means DENY, nothing else.
 */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(std::string_view caller) {
    const auto* p = reinterpret_cast<const std::byte*>(caller.data());
    return subject_token_t(p, p + caller.size());
}

std::vector<std::byte> allow_acl(std::string_view subject, std::uint32_t mask) {
    const auto* p = reinterpret_cast<const std::byte*>(subject.data());
    const tr::graph::ace_t ace{
        .type = tr::graph::ace_type_t::ALLOW,
        .flags = 0,
        .subject = std::vector<std::byte>(p, p + subject.size()),
        .access_mask = mask,
        .expires_ns = 0,
    };
    return tr::graph::encode_acl(std::span<const tr::graph::ace_t>(&ace, 1));
}

/** @brief The mask bit of one right — `acl_right_t`'s enumerators ARE the bits. */
constexpr std::uint32_t bit(acl_right_t r) { return static_cast<std::uint32_t>(r); }
constexpr std::uint8_t kMint = tr::graph::kFwdOpFlagMintRequest;
constexpr std::uint8_t kRead = static_cast<std::uint8_t>(fwd_op_t::READ);
constexpr std::uint8_t kWrite = static_cast<std::uint8_t>(fwd_op_t::WRITE);

/** @brief The raw bytes of a conformance vector's `input.bin`. */
std::vector<std::byte> vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/** @brief Byte equality over two spans. */
bool same(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// ---------------------------------------------------------------------------

void test_slot_index_is_a_bijection() {
    std::printf("the node-scoped vertex index (§6.4) — one slot per allocation, forever:\n");
    graph_t g;
    const std::size_t at_start = g.vertex_slot_count();
    check(at_start == 1, "a fresh graph holds exactly one slot: the structural root");

    const vertex_handle_t a = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    // Two vertex_t allocations: the `/sensor` placeholder and `/sensor/temp` itself.
    check(g.vertex_slot_count() == at_start + 2,
          "registering /sensor/temp appends a slot for the placeholder AND the leaf");

    const std::optional<vertex_slot_t> slot_a = g.vertex_slot(a);
    check(slot_a.has_value(), "a registered vertex has a slot");
    check(slot_a->generation == g.retire_generation(a),
          "the mint reads the index and the generation TOGETHER — one lock hold, one tenancy");
    check(g.deref_vertex_slot(slot_a->index, slot_a->generation) == a,
          "the slot dereferences back to the same vertex (index+generation is the whole check)");

    // Filling a placeholder in place must NOT hand the same object a second slot: the index
    // is per ALLOCATION, and /sensor was allocated when /sensor/temp was registered.
    const std::size_t before_fill = g.vertex_slot_count();
    const vertex_handle_t parent = g.register_vertex(path_t("/sensor"), role_t::STORED_VALUE);
    check(g.vertex_slot_count() == before_fill,
          "registering an existing placeholder appends NO slot — the object already had one");
    const std::optional<vertex_slot_t> slot_p = g.vertex_slot(parent);
    check(slot_p.has_value() && slot_p->index != slot_a->index && slot_p->index < slot_a->index,
          "the placeholder kept its own, earlier slot");

    // The index is append-only, which is the property that makes a handed-out slot permanent.
    (void)g.register_vertex(path_t("/other"), role_t::STORED_VALUE);
    check(g.vertex_slot(a) == slot_a, "a slot does not move when the graph grows");
}

void test_generation_saturates() {
    std::printf("the generation SATURATES, never wraps (§4.4 rule 3):\n");
    // Reaching the ceiling through the retire path needs 2^32 retirements of one vertex, so
    // the ceiling itself is exercised on the total function the retire path calls. The WIRING
    // — that a retire moves the counter at all — is the case below it.
    check(tr::graph::saturating_next_generation(0) == 1, "an ordinary bump advances by one");
    check(tr::graph::saturating_next_generation(tr::graph::kGenerationSaturated) ==
              tr::graph::kGenerationSaturated,
          "at the ceiling the counter STOPS — a wrapped generation would validate a stale ref");

    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    const std::uint32_t before = g.retire_generation(v);
    (void)g.retire(v);
    check(g.retire_generation(v) == before + 1, "a retire bumps the generation (the wiring)");

    // Saturation is enforced on BOTH sides, and the deref side is the load-bearing one: a mint
    // that declines only keeps this node from ISSUING a saturated element, and an element is a
    // peer-supplied number. If the deref honoured one, a saturated slot would validate the
    // same element forever — through every retire and revive, staleness detection dead for
    // that slot, which is the #603 misroute class rule 3 exists to close.
    //
    // The ceiling itself is 2^32 retirements away, so — exactly as above — the clause is
    // exercised on the total function the deref calls, whose static_asserts fail if the
    // predicate is ever written as a plain `==`.
    check(tr::graph::bound_generation_matches(3, 3), "a live element matches its slot");
    check(!tr::graph::bound_generation_matches(4, 3), "a stale element does not");
    check(!tr::graph::bound_generation_matches(tr::graph::kGenerationSaturated,
                                               tr::graph::kGenerationSaturated),
          "at the ceiling the slot and the element AGREE and the answer is still no");

    // And the wire behaviour that rides on it: a saturated element is refused at the deref
    // whatever slot it names — in range, out of range, root or leaf.
    op_resolver_t resolver(g);
    const vertex_handle_t bindable = g.register_vertex(path_t("/y"), role_t::STORED_VALUE);
    (void)g.write(bindable, make_value(b_value({0x07})));
    const vertex_slot_t live = *g.vertex_slot(bindable);
    check(resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path_ref_one(live.index, live.generation),
                                               b_path({"back"})))
              .has_value(),
          "the same slot, on its live generation, IS delivered (the ablation)");
    check(!resolve_bytes(
               resolver,
               b_fwd_raw_op(kRead, b_path_ref_one(live.index, tr::graph::kGenerationSaturated),
                            b_path({"back"})))
               .has_value(),
          "a SATURATED element on that same slot DROPS");
    check(g.vertex_slot(bindable).has_value(),
          "and the vertex is still bindable — the refusal is the element's, not the slot's");
}

void test_bound_read_matches_canonical() {
    std::printf("bound and canonical spellings of one READ deliver identical bytes (§6.3):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0xD2, 0x04, 0x00, 0x00})));

    const std::optional<vertex_slot_t> slot = g.vertex_slot(v);
    check(slot.has_value(), "the target vertex is bindable");

    const auto canonical =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path({"sensor", "temp"}), b_path({"back"})));
    const auto bound = resolve_bytes(
        resolver,
        b_fwd_raw_op(kRead, b_path_ref_one(slot->index, slot->generation), b_path({"back"})));
    check(canonical.has_value() && bound.has_value(), "both spellings answer a reply");

    const reply_facts_t cf = reply_facts(*canonical);
    const reply_facts_t bf = reply_facts(*bound);
    check(cf.kind == reply_kind_t::RESULT && bf.kind == reply_kind_t::RESULT,
          "both spellings answer kind=RESULT");
    // Not byte-identical whole frames: the reply's `src` echoes the request's `dst`, which is
    // the one thing that differs BY CONSTRUCTION between the two spellings. The payload — the
    // answer itself — is what must agree, and it is the same stored segment either way.
    const auto cb = reply_bytes(*canonical);
    const auto bb = reply_bytes(*bound);
    const std::vector<std::byte> val = b_value({0xD2, 0x04, 0x00, 0x00});
    const auto ends_with = [&](const std::vector<std::byte>& frame) {
        return frame.size() >= val.size() &&
               std::equal(val.begin(), val.end(), frame.end() - static_cast<long>(val.size()));
    };
    check(ends_with(cb) && ends_with(bb), "both replies carry the same stored value verbatim");
    check(bb.size() < cb.size(), "the bound reply is the shorter of the two (the whole point)");
}

void test_mint_round_trip() {
    std::printf("the mint exchange (§7.5) — request in the op byte, answer in the reply:\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x07})));

    const auto plain =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path({"sensor", "temp"}), b_path({"back"})));
    check(plain.has_value() && !reply_facts(*plain).has_mint,
          "an UNFLAGGED read mints nothing — the request side really is opt-in");

    const auto minted = resolve_bytes(
        resolver, b_fwd_raw_op(kRead | kMint, b_path({"sensor", "temp"}), b_path({"back"})));
    const reply_facts_t mf = reply_facts(*minted);
    check(minted.has_value() && mf.kind == reply_kind_t::RESULT,
          "a mint-flagged READ is still a READ — the opcode is `op & 0x3F` (§9.3)");
    check(mf.has_mint, "the reply carries the minted PATH_REF");
    check(
        g.vertex_slot(v) == vertex_slot_t{.index = mf.mint.index, .generation = mf.mint.generation},
        "the minted element is this node's own index + generation for the target");
    check(reply_bytes(*minted).size() == reply_bytes(*plain).size() + 12,
          "the mint costs exactly 4 + 8 bytes on the reply and zero on the request");

    // Round trip: the element the terminus answered with addresses the same vertex.
    const auto reused = resolve_bytes(
        resolver,
        b_fwd_raw_op(kRead, b_path_ref_one(mf.mint.index, mf.mint.generation), b_path({"back"})));
    check(reused.has_value() && reply_facts(*reused).kind == reply_kind_t::RESULT,
          "the minted binding addresses the same vertex on the next operation");

    // A WRITE mints too — the flag is on the op byte, not on any one opcode.
    const auto wrote =
        resolve_bytes(resolver, b_fwd_raw_op(kWrite | kMint, b_path({"sensor", "temp"}),
                                             b_path({"back"}), {}, b_value({0x09})));
    check(wrote.has_value() && reply_facts(*wrote).kind == reply_kind_t::RESULT &&
              reply_facts(*wrote).has_mint,
          "a mint-flagged WRITE writes AND mints");
}

void test_generation_mismatch_drops() {
    std::printf("a stale generation DROPS, and never delivers to the new tenant (§5.3):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/tenant/a"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x11})));
    const std::uint32_t slot = g.vertex_slot(v)->index;
    const std::uint32_t gen = g.retire_generation(v);

    // The SAME frame lands while the binding is sound — the ablation that makes every drop
    // below non-vacuous.
    const auto live =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path_ref_one(slot, gen), b_path({"back"})));
    check(live.has_value() && reply_facts(*live).kind == reply_kind_t::RESULT,
          "before the retire, this exact frame is delivered");

    // Retire, then RE-CREATE the same address for a different owner — the confused-deputy
    // shape the generation exists for.
    (void)g.retire(v);
    const vertex_handle_t revived = g.register_vertex(path_t("/tenant/a"), role_t::STORED_VALUE);
    (void)g.write(revived, make_value(b_value({0x22})));
    check(g.vertex_slot(revived)->index == slot,
          "the revived address reuses the SAME slot — the index alone cannot tell them apart");
    check(g.retire_generation(revived) != gen, "the generation moved, which is what can");

    const auto stale =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path_ref_one(slot, gen), b_path({"back"})));
    check(!stale.has_value(), "the stale binding DROPS (no reply, no delivery, no repair)");

    // And the new tenant is reachable on its own, fresh binding — so the drop above is the
    // generation talking, not the vertex having gone away.
    const auto fresh = resolve_bytes(
        resolver,
        b_fwd_raw_op(kRead, b_path_ref_one(slot, g.retire_generation(revived)), b_path({"back"})));
    check(fresh.has_value() && reply_facts(*fresh).kind == reply_kind_t::RESULT,
          "the new tenant answers on a re-minted binding");
}

void test_out_of_range_index_drops() {
    std::printf("an out-of-range index DROPS (§5.1 step 1):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x01})));

    const std::uint32_t in_range = g.vertex_slot(v)->index;
    const auto ok = resolve_bytes(
        resolver,
        b_fwd_raw_op(kRead, b_path_ref_one(in_range, g.retire_generation(v)), b_path({"back"})));
    check(ok.has_value(), "an in-range index is delivered (the ablation)");

    const auto past_end = resolve_bytes(
        resolver,
        b_fwd_raw_op(kRead, b_path_ref_one(static_cast<std::uint32_t>(g.vertex_slot_count()), 0),
                     b_path({"back"})));
    check(!past_end.has_value(), "one past the last slot DROPS");

    const auto absurd = resolve_bytes(
        resolver, b_fwd_raw_op(kRead, b_path_ref_one(0xFFFFFFFFu, 0), b_path({"back"})));
    check(!absurd.has_value(), "a peer-chosen u32 maximum DROPS rather than faulting");

    // Slot 0 is the structural root: in range, generation 0, and NOT an address.
    const auto root =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path_ref_one(0, 0), b_path({"back"})));
    check(!root.has_value(), "slot 0 (the structural root) is in range and still DROPS");
}

void test_residual_length_drops() {
    std::printf("a residual that is not exactly one element DROPS (§4.1, §5.3):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x01})));
    const path_ref_element_t good{.index = g.vertex_slot(v)->index,
                                  .generation = g.retire_generation(v)};

    const auto one = resolve_bytes(
        resolver, b_fwd_raw_op(kRead, b_path_ref(std::span<const path_ref_element_t>(&good, 1)),
                               b_path({"back"})));
    check(one.has_value(), "exactly one element is delivered (the ablation)");

    const auto empty =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path_ref({}), b_path({"back"})));
    check(!empty.has_value(),
          "a zero-element bound path DROPS — the codec admits it, the router refuses it");

    const path_ref_element_t two[2] = {good, good};
    const auto pair =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path_ref(two), b_path({"back"})));
    check(!pair.has_value(),
          "a two-element residual DROPS — this node does not forward a bound path");
}

void test_mint_denied_by_acl() {
    std::printf("a denied operation mints NOTHING (§6.1 anti-enumeration):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x05})));
    (void)g.write(path_t("/x:acl"), make_value(allow_acl("link-ok", bit(acl_right_t::READ))));

    const auto denied = resolve_bytes(
        resolver, b_fwd_raw_op(kRead | kMint, b_path({"x"}), b_path({"back"})), "link-bad");
    const reply_facts_t df = reply_facts(*denied);
    check(denied.has_value() && df.kind == reply_kind_t::ERROR && df.code == 0x0050,
          "the denied caller gets ERROR{tr::access::denied}");
    check(!df.has_mint,
          "and NO minted binding — never 'denied, and here is a handle to it' (the ablation "
          "below proves the same frame does mint when allowed)");

    const auto allowed = resolve_bytes(
        resolver, b_fwd_raw_op(kRead | kMint, b_path({"x"}), b_path({"back"})), "link-ok");
    const reply_facts_t af = reply_facts(*allowed);
    check(allowed.has_value() && af.kind == reply_kind_t::RESULT && af.has_mint,
          "the granted caller gets the value AND the binding");

    // A binding is an address, never a capability: the denied caller cannot use the granted
    // caller's binding either, because the hot path re-checks at the deref'd vertex (§6.2).
    const auto borrowed = resolve_bytes(
        resolver,
        b_fwd_raw_op(kRead, b_path_ref_one(af.mint.index, af.mint.generation), b_path({"back"})),
        "link-bad");
    const reply_facts_t bf = reply_facts(*borrowed);
    check(borrowed.has_value() && bf.kind == reply_kind_t::ERROR && bf.code == 0x0050,
          "a leaked binding is still denied — a generation match authorizes nothing");
}

void test_revocation_is_immediate() {
    std::printf("a revoked right takes effect on the next op over an existing binding (§6.2):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x05})));
    (void)g.write(path_t("/x:acl"), make_value(allow_acl("link-ok", bit(acl_right_t::READ))));

    const auto minted = resolve_bytes(
        resolver, b_fwd_raw_op(kRead | kMint, b_path({"x"}), b_path({"back"})), "link-ok");
    const reply_facts_t mf = reply_facts(*minted);
    check(mf.has_mint, "the granted caller holds a binding");

    const std::vector<std::byte> bound_read =
        b_fwd_raw_op(kRead, b_path_ref_one(mf.mint.index, mf.mint.generation), b_path({"back"}));
    const auto through = resolve_bytes(resolver, bound_read, "link-ok");
    check(through.has_value() && reply_facts(*through).kind == reply_kind_t::RESULT,
          "and reads through it");

    // Revoke by rewriting the ACL to grant someone else.
    (void)g.write(path_t("/x:acl"), make_value(allow_acl("link-other", bit(acl_right_t::READ))));
    const auto revoked = resolve_bytes(resolver, bound_read, "link-ok");
    const reply_facts_t after = revoked.has_value() ? reply_facts(*revoked) : reply_facts_t{};
    check(revoked.has_value() && after.kind == reply_kind_t::ERROR && after.code == 0x0050,
          "the very next op over the SAME binding is denied — no ACL state is cached in it");
}

void test_unmasked_op_byte_still_routes() {
    std::printf("a forwarder masks the op byte rather than switching on it (§9.3):\n");
    // peek_fwd_op is the router's terminus split (REPLY -> originator sink, request ->
    // resolve). Before the masking rule a mint-flagged READ peeked as opcode 0x80 and was
    // rejected as an unknown op at every hop — a clean error, but an error.
    const auto flagged = b_fwd_raw_op(kRead | kMint, b_path({"x"}), b_path({"back"}));
    const tr::wire::grammar::span_cursor cur{flagged};
    const std::optional<fwd_op_t> peeked = tr::net::peek_fwd_op(cur);
    check(peeked.has_value() && *peeked == fwd_op_t::READ,
          "a mint-flagged READ peeks as READ, not as an unknown opcode");

    const auto plain_reply_op = b_fwd_raw_op(static_cast<std::uint8_t>(fwd_op_t::REPLY) | kMint,
                                             b_path({"x"}), b_path({"back"}));
    const tr::wire::grammar::span_cursor rcur{plain_reply_op};
    const std::optional<fwd_op_t> rpeek = tr::net::peek_fwd_op(rcur);
    check(rpeek.has_value() && *rpeek == fwd_op_t::REPLY, "and a flagged REPLY peeks as REPLY");
}

void test_path_binding_slot() {
    std::printf("path_t keeps its value shape and gains the opaque bound slot (§7.4):\n");
    path_t p("/sensor/temp");
    check(!p.binding().bound, "a freshly parsed path is unbound");

    const path_ref_element_t els[2] = {{.index = 3, .generation = 1},
                                       {.index = 9, .generation = 4}};
    check(p.bind(els), "a two-host route binds");
    check(p.binding().bound && p.binding().elements.size() == 2, "and the elements are kept");

    // Value semantics: a copy carries the binding, and the canonical bytes survive both.
    const path_t copy = p;
    check(copy.binding() == p.binding(), "copying a path copies its binding");
    check(copy.key().size() == p.key().size(),
          "and its canonical bytes — the mint key and the "
          "fallback are never discarded");

    p.clear_binding();
    check(!p.binding().bound && p.binding().elements.empty(),
          "clearing the binding is the whole of the teardown — no hop holds anything");
    check(copy.binding().bound, "and it did not reach through to the copy");

    // Refuses past the normative bound rather than truncating: a truncated element list is a
    // valid-LOOKING binding for a different route.
    const std::vector<path_ref_element_t> too_many(tr::wire::kMaxPathRefElements + 1);
    check(!p.bind(too_many) && !p.binding().bound, "a route past the element cap does not bind");
    check(!p.bind({}) && !p.binding().bound, "and neither does an empty one");
}

/**
 * @brief The four RFC-0024 behavioural vectors, as BYTES, against the reference impl.
 *
 * A vector gates the codec and only the codec (HARNESS.md §"What a vector gates"), so every
 * behavioural claim a `description.md` makes has to be made somewhere it can be false. This
 * is that place for the mint exchange and the §6.3 pair: each vector's bytes are fed to — or
 * compared against — the resolver, so a vector that drifts from the code fails here rather
 * than sitting on disk describing a protocol nobody implements.
 *
 * The graph is the one the vectors were taken from: `/sensor/temp` in an empty node, which
 * allocates the `/sensor` placeholder at slot 1 and the leaf at slot 2 under the structural
 * root at slot 0 — hence `index = 2`, `generation = 0`.
 */
void test_conformance_vectors() {
    std::printf("the RFC-0024 vectors, byte-exact against the resolver:\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0xD2, 0x04, 0x00, 0x00})));
    check(g.vertex_slot(v) == vertex_slot_t{.index = 2u, .generation = 0u},
          "the vectors' index=2 generation=0 is what this graph really hands out");

    // fwd/fwd-mint-request differs from fwd/fwd-read in exactly one byte.
    const std::vector<std::byte> mint_req = vector_bytes("fwd/fwd-mint-request");
    const std::vector<std::byte> plain_req = vector_bytes("fwd/fwd-read");
    std::size_t differing = 0;
    for (std::size_t i = 0; i < mint_req.size() && i < plain_req.size(); ++i)
        if (mint_req[i] != plain_req[i]) ++differing;
    check(mint_req.size() == plain_req.size() && differing == 1,
          "fwd-mint-request is fwd-read with ONE byte changed — the request costs zero bytes");

    // ...and the reply to it is fwd/fwd-mint-reply, byte for byte.
    const auto minted = resolve_bytes(resolver, mint_req);
    check(minted.has_value() && same(reply_bytes(*minted), vector_bytes("fwd/fwd-mint-reply")),
          "fwd-mint-reply is byte-exact what the resolver emits for fwd-mint-request");

    // The §6.3 pair, allow half: the bound spelling serves what the canonical one serves.
    const auto bound = resolve_bytes(resolver, vector_bytes("acl/bound-vs-canonical-allow"));
    const auto canonical = resolve_bytes(resolver, plain_req);
    check(bound.has_value() && canonical.has_value(), "both spellings of the pair answer");
    const std::vector<std::byte> bb = reply_bytes(*bound);
    const std::vector<std::byte> cb = reply_bytes(*canonical);
    const std::vector<std::byte> val = b_value({0xD2, 0x04, 0x00, 0x00});
    const auto tail = [&](const std::vector<std::byte>& f) {
        return std::vector<std::byte>(f.end() - static_cast<long>(val.size()), f.end());
    };
    check(tail(bb) == val && tail(cb) == val,
          "allow half: the two spellings serve byte-identical RESULT bytes");

    // The §6.3 pair, deny half: the same request, denied, IS the vector's frame.
    graph_t gd;
    gd.set_subject_resolver(caller_is_subject);
    op_resolver_t rd(gd);
    const vertex_handle_t vd = gd.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)vd;
    (void)gd.write(path_t("/sensor/temp:acl"),
                   make_value(allow_acl("link-ok", bit(acl_right_t::READ))));
    const auto denied = resolve_bytes(rd, vector_bytes("acl/bound-vs-canonical-allow"), "link-bad");
    check(denied.has_value() &&
              same(reply_bytes(*denied), vector_bytes("acl/bound-vs-canonical-deny")),
          "deny half: bound-vs-canonical-deny is byte-exact what the denied bound READ emits");

    // ...and the canonical spelling's denial agrees on the OUTCOME, which is the claim.
    const auto denied_canonical = resolve_bytes(rd, plain_req, "link-bad");
    const std::vector<std::byte> dv = reply_bytes(*denied);
    const std::vector<std::byte> dc = reply_bytes(*denied_canonical);
    constexpr std::size_t kOutcome = 19;  // kind VALUE (5) + STATUS{ERROR{VALUE u16}} (14)
    check(denied_canonical.has_value() && dv.size() > kOutcome && dc.size() > kOutcome &&
              std::equal(dv.end() - kOutcome, dv.end(), dc.end() - kOutcome),
          "deny half: the two spellings' outcome tails are byte-identical");
    check(dv.size() != dc.size(),
          "and they differ ONLY in the reply src, which echoes the request dst");
    check(!reply_facts(*denied).has_mint, "a denied reply carries no minted binding");
}

}  // namespace

int main() {
    test_slot_index_is_a_bijection();
    test_generation_saturates();
    test_bound_read_matches_canonical();
    test_mint_round_trip();
    test_generation_mismatch_drops();
    test_out_of_range_index_drops();
    test_residual_length_drops();
    test_mint_denied_by_acl();
    test_revocation_is_immediate();
    test_unmasked_op_byte_still_routes();
    test_path_binding_slot();
    test_conformance_vectors();
    return tr::testing::summary("bound_path");
}
