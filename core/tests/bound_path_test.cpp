/**
 * @file
 * @brief RFC-0024 — bound-path routing, and the RFC-0029 §6.2 PAIR a terminus answers with.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The behavioural half of RFC-0024: what a bound address MEANS, as opposed to what its bytes
 * are (`path_ref_test.cpp` owns the codec). RFC-0029 S1 re-spells the bound `dst`: the
 * element now rides INSIDE a `PATH` as a PAIR (`00 16 08 <idx><gen>`), and a `PATH_REF`
 * (`0x14`) is no longer an address — the terminus refuses it `INVALID_PATH`. Everything here is
 * arranged around one property — **a bound path never mis-routes**. Each way validation can fail
 * gets a case that actually reaches it and a case that proves the same frame WOULD have been
 * delivered with a sound element, so no guard here can be satisfied vacuously:
 *
 * - a stale generation drops, and the retired-then-revived address proves it drops rather
 *   than delivering into the new tenant;
 * - an out-of-range index drops, against an in-range one that lands;
 * - a residual that is not exactly one element drops, at both ends of the range;
 * - the PAIR a success reply's `src` carries is withheld wherever the operation is refused,
 *   so a denied caller learns nothing;
 * - and the bound and canonical spellings of one operation produce byte-identical replies.
 *
 * Since #2245 nothing asks for the PAIR: every successful terminus reply seeds its `src` with
 * it, and the RFC-0024 op-byte flag that used to ask is an undefined opcode bit, refused.
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
#include "graph_sinks.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
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
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A `PATH_REF` over @p elements — the RETIRED bound spelling, refused as a `dst`. */
std::vector<std::byte> b_path_ref(std::span<const path_ref_element_t> elements) {
    std::vector<std::byte> out;
    (void)tr::wire::emit_path_ref(out, elements);
    return out;
}

/**
 * @brief The one-element bound spelling a terminus sees after every hop has consumed its own:
 *        a `PATH` holding one PAIR element (RFC-0029 §4).
 */
std::vector<std::byte> b_path_ref_one(std::uint32_t index, std::uint32_t generation) {
    std::vector<std::byte> body;
    tr::testing::emit_path_pair(body,
                                tr::wire::path_pair_t{.index = index, .generation = generation});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

std::vector<std::byte> b_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> p;
    for (std::uint8_t b : bytes) p.push_back(std::byte{b});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

using tr::testing::b_fwd_raw_op;

/**
 * @brief Arena-decode + resolve — the terminus wiring `fwd_router_t` uses.
 *
 * A `dst` that is exactly one PAIR element is handed to the resolver as its deref target,
 * which is what `fwd_router_t::route_pair_forward` does when the element is the last one
 * (RFC-0029 §6 step 3, TERMINUS). Every other `dst` resolves on its own bytes.
 */
tr::graph::result_t<tr::view::rope_t> resolve_bytes(op_resolver_t& resolver,
                                                    std::span<const std::byte> fwd,
                                                    std::string_view inbound_link = {}) {
    const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
    if (!arena) return std::unexpected(tr::graph::status_t::INVALID_PATH);
    std::optional<tr::wire::path_pair_t> target;
    if (const auto dec = tr::wire::decode(fwd); dec && dec->children.size() > 1) {
        const tlv_t& dst = dec->children[1];
        if (dst.type == type_t::PATH && dst.payload.size() == tr::wire::kPathPairRecordBytes)
            target = tr::wire::path_pair_at(dst.payload, 0);
    }
    return resolver.resolve(*arena, inbound_link, nullptr, target ? &*target : nullptr);
}

/** @brief The flattened reply bytes (the consumer's one allowed copy). */
std::vector<std::byte> reply_bytes(const tr::view::rope_t& reply) {
    const tr::view::view_t flat = reply.flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

/** @brief What a decoded reply says: its kind, its error identity, and the PAIR its `src` holds. */
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
    // The terminus's PAIR is the reply's `src` (index 2), seeded on success (RFC-0029 §6.2):
    // a PATH of exactly one PAIR record. An error keeps the residual NAMEs instead.
    const tlv_t& src = dec->children[2];
    if (src.type == type_t::PATH && src.payload.size() == tr::wire::kPathPairRecordBytes) {
        if (const auto pair = tr::wire::path_pair_at(src.payload, 0)) {
            out.has_mint = true;
            out.mint = path_ref_element_t{.index = pair->index, .generation = pair->generation};
        }
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
std::expected<void, tr::wire::err_t> caller_is_subject(void*, std::string_view caller,
                                                       tr::mem::bytes_t& out, std::uint64_t&) {
    const auto* p = reinterpret_cast<const std::byte*>(caller.data());
    return tr::testing::subject_into(out, std::vector<std::byte>(p, p + caller.size()));
}

std::vector<std::byte> allow_acl(std::string_view subject, std::uint32_t mask) {
    const auto* p = reinterpret_cast<const std::byte*>(subject.data());
    const tr::graph::ace_t ace{
        .type = tr::graph::ace_type_t::ALLOW,
        .flags = 0,
        .subject = std::span<const std::byte>(p, subject.size()),
        .access_mask = mask,
        .expires_ns = 0,
    };
    return tr::graph::encode_acl(std::span<const tr::graph::ace_t>(&ace, 1));
}

/** @brief The mask bit of one right — `acl_right_t`'s enumerators ARE the bits. */
constexpr std::uint32_t bit(acl_right_t r) { return static_cast<std::uint32_t>(r); }
/** @brief The retired RFC-0024 mint flag's bit — an undefined opcode bit since #2245. */
constexpr std::uint8_t kBit7 = 0x80;
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

/**
 * @brief The #1486 memo answers what the scan answered — for EVERY slot, not a sampled one.
 *
 * `vertex_slot` no longer scans: each vertex carries the index of its own slot, stamped once
 * when the slot was appended. A memo is only ever as good as its agreement with the thing it
 * replaced, so this walks the index in BOTH directions over a population big enough to have
 * caught an off-by-one, a stale stamp, or a placeholder/leaf mix-up, and requires the round
 * trip to be the identity at every single slot:
 *
 *   slot i --deref--> handle --vertex_slot--> i
 *
 * Placeholders are included by construction (the deep paths below allocate them), as are the
 * two vertices a `graph_t` seeds itself with. The forward leg (`vertex_slot_at`) reads the
 * deque by index and never consults the memo, so it is the independent side of the check.
 */
void test_slot_memo_agrees_with_the_scan() {
    std::printf("the reverse index is memoized, and the memo is the scan's answer (#1486):\n");
    graph_t g;
    // Deep and wide: every level allocates a placeholder, so the slot order interleaves
    // placeholders with registered leaves rather than being a run of one kind.
    for (int i = 0; i < 40; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "/bank%d/rack%d/cell%d", i % 5, i % 7, i);
        (void)g.register_vertex(path_t(buf), role_t::STORED_VALUE);
    }
    const std::size_t slots = g.vertex_slot_count();
    check(slots > 40, "the population includes placeholders as well as the registered leaves");

    std::size_t round_tripped = 0;
    for (std::uint32_t i = 0; i < slots; ++i) {
        const std::optional<vertex_slot_t> at = g.vertex_slot_at(i);
        if (!at.has_value()) continue;  // an unregistered placeholder — not mintable, §7.1
        const std::optional<vertex_handle_t> vh = g.deref_vertex_slot(at->index, at->generation);
        check(vh.has_value(), "a slot the forwarder could mint for dereferences");
        const std::optional<vertex_slot_t> back = g.vertex_slot(*vh);
        check(back.has_value() && back->index == i,
              "the memo answers the slot the vertex is actually AT — every slot, both ways");
        ++round_tripped;
    }
    check(round_tripped >= 40, "the round trip covered the whole registered population");

    // A vertex no graph ever slotted keeps working: it has no memo, and the fallback scan
    // reports what it always did — nothing. This is the case the re-validation exists for.
    graph_t other;
    const vertex_handle_t foreign =
        other.register_vertex(path_t("/elsewhere"), role_t::STORED_VALUE);
    check(!g.vertex_slot(foreign).has_value(),
          "a foreign vertex resolves to no slot in this graph (the memo is re-validated, "
          "never trusted)");
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
    // Byte-identical whole frames: a success reply's `src` is the target's PAIR whichever way
    // the request spelled its `dst` (RFC-0029 §6.2), and the payload is the same stored segment.
    const auto cb = reply_bytes(*canonical);
    const auto bb = reply_bytes(*bound);
    const std::vector<std::byte> val = b_value({0xD2, 0x04, 0x00, 0x00});
    const auto ends_with = [&](const std::vector<std::byte>& frame) {
        return frame.size() >= val.size() &&
               std::equal(val.begin(), val.end(), frame.end() - static_cast<long>(val.size()));
    };
    check(ends_with(cb) && ends_with(bb), "both replies carry the same stored value verbatim");
    check(bb == cb, "and the two replies are byte-identical — both answer with the PAIR");
}

void test_success_reply_carries_the_pair() {
    std::printf("every success reply's src is the target's PAIR, unasked (RFC-0029 §6.2):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x07})));

    const auto plain =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path({"sensor", "temp"}), b_path({"back"})));
    const reply_facts_t mf = plain ? reply_facts(*plain) : reply_facts_t{};
    check(plain.has_value() && mf.kind == reply_kind_t::RESULT, "a plain READ answers RESULT");
    check(mf.has_mint, "and its src is a PATH of one PAIR — no flag asked for it");
    check(
        g.vertex_slot(v) == vertex_slot_t{.index = mf.mint.index, .generation = mf.mint.generation},
        "the PAIR is this node's own index + generation for the target");

    // Round trip: the element the terminus answered with addresses the same vertex.
    const auto reused = resolve_bytes(
        resolver,
        b_fwd_raw_op(kRead, b_path_ref_one(mf.mint.index, mf.mint.generation), b_path({"back"})));
    check(reused.has_value() && reply_facts(*reused).kind == reply_kind_t::RESULT,
          "the learned PAIR addresses the same vertex on the next operation");

    // A WRITE's acknowledgement carries it too — the seed is the reply's, not one opcode's.
    const auto wrote = resolve_bytes(resolver, b_fwd_raw_op(kWrite, b_path({"sensor", "temp"}),
                                                            b_path({"back"}), {}, b_value({0x09})));
    check(wrote.has_value() && reply_facts(*wrote).kind == reply_kind_t::RESULT &&
              reply_facts(*wrote).has_mint,
          "a WRITE writes AND answers with the PAIR");

    // An error keeps the residual NAMEs: a route that does not resolve has no PAIR to give.
    const auto missing =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path({"sensor", "nope"}), b_path({"back"})));
    check(missing.has_value() && reply_facts(*missing).kind == reply_kind_t::ERROR &&
              !reply_facts(*missing).has_mint,
          "a NOT_FOUND carries no PAIR");
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
    std::printf("a PATH_REF dst is refused INVALID_PATH, whatever its length (RFC-0029 S1):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x01})));
    const path_ref_element_t good{.index = g.vertex_slot(v)->index,
                                  .generation = g.retire_generation(v)};

    const auto pair = resolve_bytes(
        resolver,
        b_fwd_raw_op(kRead, b_path_ref_one(good.index, good.generation), b_path({"back"})));
    check(pair.has_value() && reply_facts(*pair).kind == reply_kind_t::RESULT,
          "the same element spelled as a PAIR is delivered (the ablation)");

    const path_ref_element_t two[2] = {good, good};
    const std::span<const path_ref_element_t> lengths[] = {
        {}, std::span<const path_ref_element_t>(&good, 1), two};
    for (const std::span<const path_ref_element_t> els : lengths) {
        const auto r =
            resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path_ref(els), b_path({"back"})));
        check(!r.has_value() && r.error() == tr::graph::status_t::INVALID_PATH,
              "a 0x14 dst is refused INVALID_PATH — it is no longer an address");
    }
}

void test_mint_denied_by_acl() {
    std::printf("a denied operation mints NOTHING (§6.1 anti-enumeration):\n");
    graph_t g;
    g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {caller_is_subject, nullptr});
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x05})));
    (void)g.write(path_t("/x:acl"), make_value(allow_acl("link-ok", bit(acl_right_t::READ))));

    const auto denied =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path({"x"}), b_path({"back"})), "link-bad");
    const reply_facts_t df = reply_facts(*denied);
    check(denied.has_value() && df.kind == reply_kind_t::ERROR && df.code == 0x0050,
          "the denied caller gets ERROR{tr::access::denied}");
    check(!df.has_mint,
          "and NO PAIR in its src — never 'denied, and here is a handle to it' (the ablation "
          "below proves the same frame does mint when allowed)");

    const auto allowed =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path({"x"}), b_path({"back"})), "link-ok");
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
    g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {caller_is_subject, nullptr});
    op_resolver_t resolver(g);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(b_value({0x05})));
    (void)g.write(path_t("/x:acl"), make_value(allow_acl("link-ok", bit(acl_right_t::READ))));

    const auto minted =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path({"x"}), b_path({"back"})), "link-ok");
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

void test_bit7_is_refused() {
    std::printf("a set op-byte bit 7 is an undefined opcode at the terminus (#2245):\n");
    graph_t g;
    op_resolver_t resolver(g);
    const vertex_handle_t x = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(x, make_value(b_value({0x01})));
    // A forwarder masks the op byte rather than switching on it (§9.3), so a peer's stray bit
    // still routes to the terminus that owns the decision...
    const auto flagged = b_fwd_raw_op(kRead | kBit7, b_path({"x"}), b_path({"back"}));
    const tr::wire::grammar::span_cursor cur{flagged};
    const std::optional<fwd_op_t> peeked = tr::net::peek_fwd_op(cur);
    check(peeked.has_value() && *peeked == fwd_op_t::READ, "a bit-7 READ peeks as READ at a hop");
    // ...and the terminus refuses it: the RFC-0024 mint flag is retired, not ignored.
    const auto r = resolve_bytes(resolver, flagged);
    check(r.has_value() && reply_facts(*r).kind == reply_kind_t::ERROR && !reply_facts(*r).has_mint,
          "the terminus answers kind=ERROR, and with no PAIR");
    // The ablation: the same frame without the bit answers.
    const auto ok = resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path({"x"}), b_path({"back"})));
    check(ok.has_value() && reply_facts(*ok).kind == reply_kind_t::RESULT,
          "the same READ without the bit answers RESULT");
}

/**
 * @brief RFC-0024 §7.1 amendment 2: the reverse list is told from the payload by its own TYPE
 *        (`PATH_REF_REVERSE`, `0x15`), never by its position.
 *
 * Two claims, and the first is the one amendment 1's positional rule got wrong. Under that
 * rule a WRITE whose stored value happened to be a raw `PATH_REF` TLV was read
 * as a reverse list and the write lost its payload — a shape with no in-tree producer, which
 * is exactly why the foreclosure was cheap to take and is now cheap to give back. The second
 * claim is the one the rule got right and the type keeps: a genuine trailing reverse list is
 * NOT swallowed as the WRITE's payload.
 */
void test_reverse_list_is_typed_not_positional() {
    std::printf("the reverse list is identified by TYPE, not by position (§7.1 amendment 2):\n");
    graph_t g;
    op_resolver_t resolver(g);
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    const auto ends_with = [](const std::vector<std::byte>& frame,
                              const std::vector<std::byte>& tail) {
        return frame.size() >= tail.size() &&
               std::equal(tail.begin(), tail.end(), frame.end() - static_cast<long>(tail.size()));
    };
    const auto read_back = [&]() {
        const auto r = resolve_bytes(
            resolver, b_fwd_raw_op(kRead, b_path({"sensor", "temp"}), b_path({"back"})));
        return r.has_value() ? reply_bytes(*r) : std::vector<std::byte>{};
    };

    // (1) A WRITE whose PAYLOAD is a raw `PATH_REF` TLV — un-foreclosed.
    const std::vector<std::byte> ref_payload = b_path_ref_one(7u, 3u);
    const auto wrote_ref = resolve_bytes(resolver, b_fwd_raw_op(kWrite, b_path({"sensor", "temp"}),
                                                                b_path({"back"}), {}, ref_payload));
    check(wrote_ref.has_value() && reply_facts(*wrote_ref).kind == reply_kind_t::RESULT,
          "a WRITE whose payload is a raw PATH_REF still writes");
    check(ends_with(read_back(), ref_payload),
          "and the raw PATH_REF is what got STORED — the payload is no longer eaten by a "
          "positional reverse-list rule");

    // (2) A WRITE carrying BOTH a payload and a genuine reverse list — a PATH body of one PAIR
    // record (RFC-0029 §7.1).
    std::vector<std::byte> rev_body;
    tr::testing::emit_path_pair(rev_body, tr::wire::path_pair_t{.index = 11u, .generation = 2u});
    std::vector<std::byte> reverse_child;
    tr::wire::emit_tlv(reverse_child, type_t::PATH_REF_REVERSE, opt_t{}, rev_body);
    check(
        !reverse_child.empty() && static_cast<type_t>(reverse_child[0]) == type_t::PATH_REF_REVERSE,
        "and it heads with 0x15, not 0x14");
    const std::vector<std::byte> value_payload = b_value({0x2A});
    std::vector<std::byte> tail = value_payload;
    tail.insert(tail.end(), reverse_child.begin(), reverse_child.end());
    const auto wrote_both = resolve_bytes(
        resolver, b_fwd_raw_op(kWrite, b_path({"sensor", "temp"}), b_path({"back"}), {}, tail));
    check(wrote_both.has_value() && reply_facts(*wrote_both).kind == reply_kind_t::RESULT,
          "a WRITE carrying payload AND reverse list writes");
    const std::vector<std::byte> stored = read_back();
    check(ends_with(stored, value_payload),
          "the VALUE is what got stored — the reverse child is not the payload");
    check(!ends_with(stored, reverse_child), "and the reverse list was not stored as one");
}

void test_path_binding_slot() {
    std::printf("path_t keeps its value shape and gains the opaque bound slot (§7.4):\n");
    path_t p("/sensor/temp");
    check(!p.binding().bound, "a freshly parsed path is unbound");

    std::vector<std::byte> chain;
    tr::testing::emit_path_pair(chain, tr::wire::path_pair_t{.index = 3, .generation = 1});
    tr::testing::emit_path_pair(chain, tr::wire::path_pair_t{.index = 9, .generation = 4});
    check(p.bind(chain), "a two-host chain binds");
    check(p.binding().bound && p.binding().chain.size() == 2 * tr::wire::kPathPairRecordBytes,
          "and the chain is kept verbatim");

    // Value semantics: a copy carries the binding, and the canonical bytes survive both.
    const path_t copy = p;
    check(copy.binding() == p.binding(), "copying a path copies its binding");
    check(copy.key().size() == p.key().size(),
          "and its canonical bytes — the fallback is never discarded");

    p.clear_binding();
    check(!p.binding().bound && p.binding().chain.empty(),
          "clearing the binding is the whole of the teardown — no hop holds anything");
    check(copy.binding().bound, "and it did not reach through to the copy");

    // Refuses past a 16-bit PATH body rather than truncating: a truncated chain is a
    // valid-LOOKING binding for a different route.
    const std::vector<std::byte> too_long(0x10000);
    check(!p.bind(too_long) && !p.binding().bound, "a chain past a 16-bit body does not bind");
    check(!p.bind({}) && !p.binding().bound, "and neither does an empty one");
}

/**
 * @brief The RFC-0024 behavioural vectors still standing, as BYTES, against the reference impl.
 *
 * A vector gates the codec and only the codec (HARNESS.md §"What a vector gates"), so every
 * behavioural claim a `description.md` makes has to be made somewhere it can be false. This
 * is that place for the PAIR a read answers with and the §6.3 pair: each vector's bytes are fed to
 * — or compared against — the resolver, so a vector that drifts from the code fails here rather
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

    // The RFC-0024 mint vectors and the 0x14-dst §6.3 pair retired with the flag (#2245): the
    // PAIR now rides every success reply's `src`, and the pair is re-bound on the PAIR spelling
    // in path_pair_test.
    const std::vector<std::byte> plain_req = vector_bytes("fwd/fwd-read");
    const auto learned = resolve_bytes(resolver, plain_req);
    const reply_facts_t lf = learned ? reply_facts(*learned) : reply_facts_t{};
    check(lf.has_mint && lf.mint.index == 2u && lf.mint.generation == 0u,
          "fwd-read's reply src is the PAIR index=2 generation=0");
    const auto bound =
        resolve_bytes(resolver, b_fwd_raw_op(kRead, b_path_ref_one(2u, 0u), b_path({"reply-ep"})));
    const auto canonical = resolve_bytes(resolver, plain_req);
    const std::vector<std::byte> val = b_value({0xD2, 0x04, 0x00, 0x00});
    const auto tail = [&](const std::vector<std::byte>& f) {
        return std::vector<std::byte>(f.end() - static_cast<long>(val.size()), f.end());
    };
    check(bound.has_value() && canonical.has_value() && tail(reply_bytes(*bound)) == val &&
              tail(reply_bytes(*canonical)) == val,
          "the PAIR spelling of the same READ serves the canonical RESULT bytes");
}

}  // namespace

int main() {
    test_slot_index_is_a_bijection();
    test_slot_memo_agrees_with_the_scan();
    test_generation_saturates();
    test_bound_read_matches_canonical();
    test_success_reply_carries_the_pair();
    test_generation_mismatch_drops();
    test_out_of_range_index_drops();
    test_residual_length_drops();
    test_mint_denied_by_acl();
    test_revocation_is_immediate();
    test_bit7_is_refused();
    test_reverse_list_is_typed_not_positional();
    test_path_binding_slot();
    test_conformance_vectors();
    return tr::testing::summary("bound_path");
}
