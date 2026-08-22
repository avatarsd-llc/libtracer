/**
 * @file
 * @brief RFC-0025 §4.1.2 (Amendment 3, clause 5) — `propagate`'s FOLD emission mode.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A folded sweep emits ONE branch-write frame for the subtree it swept instead of RFC-0008
 * §D's one `FWD{WRITE}` per selected vertex. What is asserted here, in the order the ruling
 * states it: the DEFAULT emission does not move; the fold is one frame where the default is N;
 * per-leaf values are BYTE-IDENTICAL under both modes (the differential that makes "the same
 * truth, less framing" checkable rather than asserted); the frame's root carries its leading
 * `NAME` per RFC-0005 §B; a trailer-carrying node is REJECTED rather than silently stripped;
 * a selected STREAM is refused, because a since-flush LIST has no seat on a §B node; and two
 * disjoint subtrees are TWO frames and never a container (the retired-LIST ban of RFC-0005 §E,
 * made executable). A refusal consumes nothing, so the caller may always retry `PER_VERTEX`.
 *
 * Nothing here touches the receive path, because clause 5 requires that it need not: §B's
 * existing slicing is what hands each covered subscriber its subview.
 *
 * @par Callback lifetimes
 * `subscribe` binds a callback BY ADDRESS and the graph's dispatch reads it, so every log and
 * every lambda below is declared BEFORE the graph that points at it — reverse destruction then
 * tears the graph down first. `unsubscribe` is not a barrier (ADR-0080 §Decision 4), so
 * outliving the graph is the only ordering that is actually safe (#1484 / #1489).
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::emission_mode_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief One recorded delivery: the bytes a subscriber was handed, verbatim. */
using log_t = std::vector<std::vector<std::byte>>;

/** @brief A whole rope's bytes, gathered — a folded frame is scatter-gather by construction. */
std::vector<std::byte> bytes_of(const rope_t& r) {
    std::vector<std::byte> out;
    r.walk([&out](std::span<const std::byte> s) { out.insert(out.end(), s.begin(), s.end()); });
    return out;
}

/** @brief A `VALUE` TLV over @p payload — the shape a §B branch-write node admits. */
std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> payload) {
    std::vector<std::byte> body;
    for (const std::uint8_t b : payload) body.push_back(std::byte{b});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, body);
    return out;
}

/**
 * @brief A `VALUE` TLV carrying an absolute-timestamp TRAILER (`opt.TS=1, TF=0`) — hand-built,
 *        because the reference emitter deliberately mints no trailer (#1109).
 */
std::vector<std::byte> timestamped_value_tlv(std::uint8_t payload) {
    std::vector<std::byte> out{std::byte{0x01}, std::byte{0x20}, std::byte{0x01}, std::byte{0x00},
                               std::byte{payload}};
    for (int i = 0; i < 8; ++i) out.push_back(std::byte{0});  // the u64 LE ns trailer
    return out;
}

/**
 * @brief The `NAME` segment text at the head of a `POINT` TLV's body, or empty when @p frame is
 *        not a `POINT` whose first child is a `NAME`.
 *
 * Deliberately a hand walk over the header bytes rather than a decode: what is under test IS
 * the emitted framing, so reading it back through the arena the emitter mirrors would make
 * this assertion agree with itself instead of with the spec.
 */
std::string root_name_of(std::span<const std::byte> frame) {
    if (frame.size() < 8) return {};
    if (frame[0] != std::byte{0x07}) return {};                             // POINT
    if ((std::to_integer<std::uint8_t>(frame[1]) & 0x40U) == 0) return {};  // opt.PL
    const std::size_t hdr = (std::to_integer<std::uint8_t>(frame[1]) & 0x08U) != 0 ? 6U : 4U;
    if (frame.size() < hdr + 4 || frame[hdr] != std::byte{0x02}) return {};  // NAME
    const std::size_t n =
        static_cast<std::size_t>(std::to_integer<std::uint8_t>(frame[hdr + 2])) |
        (static_cast<std::size_t>(std::to_integer<std::uint8_t>(frame[hdr + 3])) << 8U);
    if (frame.size() < hdr + 4 + n) return {};
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<char>(std::to_integer<std::uint8_t>(frame[hdr + 4 + i])));
    }
    return out;
}

/** @brief True iff @p needle appears contiguously inside @p hay. */
bool contains(std::span<const std::byte> hay, std::span<const std::byte> needle) {
    return std::ranges::search(hay, needle).begin() != hay.end();
}

/**
 * @brief `/s`, `/s/mid`, `/s/mid/leaf`, `/s/other` — a two-level subtree with a sibling.
 *
 * Subscriptions must be taken BEFORE the assigns: `mark_pending` skips an unobserved vertex on
 * purpose (the idle-write fast path), so a sweep over a graph nobody watches selects nothing
 * and every assertion below would pass vacuously.
 */
struct fixture_t {
    graph_t g;
    vertex_handle_t s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    vertex_handle_t mid = g.register_vertex(path_t("/s/mid"), role_t::STORED_VALUE);
    vertex_handle_t leaf = g.register_vertex(path_t("/s/mid/leaf"), role_t::STORED_VALUE);
    vertex_handle_t other = g.register_vertex(path_t("/s/other"), role_t::STORED_VALUE);
};

// --- tests -------------------------------------------------------------------

void test_default_emission_unchanged() {
    std::printf("the default emission does not move (RFC-0008 §D stays the floor):\n");
    log_t at_one;
    log_t at_two;
    auto on_one = [&at_one](const rope_t& v) { at_one.push_back(bytes_of(v)); };
    auto on_two = [&at_two](const rope_t& v) { at_two.push_back(bytes_of(v)); };
    fixture_t one;
    fixture_t two;
    check(one.g.subscribe(path_t("/s"), on_one).has_value(),
          "subscribe at the sweep root of the one-arg arm");
    check(two.g.subscribe(path_t("/s"), on_two).has_value(),
          "subscribe at the sweep root of the PER_VERTEX arm");
    const std::vector<std::byte> a = value_tlv({0xAA});
    const std::vector<std::byte> b = value_tlv({0xBB});
    check(one.g.assign(one.leaf, make_value(a)).has_value(), "assign the leaf (one-arg arm)");
    check(one.g.assign(one.other, make_value(b)).has_value(), "assign the sibling (one-arg arm)");
    check(two.g.assign(two.leaf, make_value(a)).has_value(), "assign the leaf (PER_VERTEX arm)");
    check(two.g.assign(two.other, make_value(b)).has_value(), "assign the sibling (PER_VERTEX)");

    (void)one.g.propagate(one.s);
    check(two.g.propagate(two.s, emission_mode_t::PER_VERTEX).has_value(),
          "PER_VERTEX propagate succeeds");
    check(at_one == at_two,
          "propagate(v, PER_VERTEX) delivers byte-for-byte what propagate(v) delivers");
    check(at_one.size() == 2, "the default is still one delivery per selected vertex");
}

void test_fold_is_one_frame_with_the_same_leaf_truth() {
    std::printf("one branch-write frame per sweep, same per-leaf truth (clause 5):\n");
    const std::vector<std::byte> a = value_tlv({0xAA, 0x01});
    const std::vector<std::byte> b = value_tlv({0xBB, 0x02});

    log_t root_default;
    log_t leaf_default;
    log_t root_fold;
    log_t leaf_fold;
    auto on_rd = [&root_default](const rope_t& v) { root_default.push_back(bytes_of(v)); };
    auto on_ld = [&leaf_default](const rope_t& v) { leaf_default.push_back(bytes_of(v)); };
    auto on_rf = [&root_fold](const rope_t& v) { root_fold.push_back(bytes_of(v)); };
    auto on_lf = [&leaf_fold](const rope_t& v) { leaf_fold.push_back(bytes_of(v)); };
    // Two arms are two graphs, so one arm's sweep state cannot bleed into the other's.
    fixture_t d;
    fixture_t f;
    check(d.g.subscribe(path_t("/s"), on_rd).has_value(), "subtree subscriber, default arm");
    check(d.g.subscribe(path_t("/s/mid/leaf"), on_ld).has_value(), "leaf subscriber, default arm");
    check(f.g.subscribe(path_t("/s"), on_rf).has_value(), "subtree subscriber, fold arm");
    check(f.g.subscribe(path_t("/s/mid/leaf"), on_lf).has_value(), "leaf subscriber, fold arm");
    for (fixture_t* fx : {&d, &f}) {
        check(fx->g.assign(fx->leaf, make_value(a)).has_value(), "assign /s/mid/leaf");
        check(fx->g.assign(fx->other, make_value(b)).has_value(), "assign /s/other");
    }

    (void)d.g.propagate(d.s);
    check(f.g.propagate(f.s, emission_mode_t::FOLD).has_value(), "the folded sweep succeeds");

    // THE framing claim. The default fans the root subscriber one delivery per selected
    // vertex; the fold hands it exactly one, and that one carries both leaves' bytes.
    check(root_default.size() == 2, "default: root sees one delivery per selected vertex");
    check(root_fold.size() == 1, "FOLD: the root sees exactly ONE frame for the whole sweep");
    check(root_fold.size() == 1 && contains(root_fold[0], a),
          "the folded frame carries /s/mid/leaf's VALUE verbatim");
    check(root_fold.size() == 1 && contains(root_fold[0], b),
          "the folded frame carries /s/other's VALUE verbatim");

    // THE differential. The per-leaf truth a covered subscriber sees is the same bytes under
    // both emissions — the fold changes framing, never the value that lands.
    check(leaf_default == leaf_fold,
          "byte-identical per-leaf values under FOLD and the per-vertex default");
    check(leaf_fold.size() == 1 && leaf_fold[0] == a, "and those bytes are the leaf's own VALUE");

    // THE root shape. A branch-write root carries its leading NAME (RFC-0005 §B) — the one
    // root asymmetry RFC-0016 §A names against a composed-READ root.
    check(root_fold.size() == 1 && root_name_of(root_fold[0]) == "s",
          "the folded frame's root POINT carries the target's leaf segment as its NAME");
}

void test_trailer_carrying_node_is_rejected() {
    std::printf("a trailer-carrying node is REJECTED inside a branch write (§B strictness):\n");
    log_t at_root;
    auto on_root = [&at_root](const rope_t& v) { at_root.push_back(bytes_of(v)); };
    fixture_t fx;
    check(fx.g.subscribe(path_t("/s"), on_root).has_value(), "subtree subscriber at /s");
    check(fx.g.assign(fx.leaf, make_value(timestamped_value_tlv(0x77))).has_value(),
          "assign a trailer-carrying VALUE at the leaf");

    const auto folded = fx.g.propagate(fx.s, emission_mode_t::FOLD);
    check(!folded && folded.error() == status_t::TYPE_MISMATCH,
          "FOLD refuses the sweep with TYPE_MISMATCH rather than stripping the trailer");
    check(at_root.empty(), "and nothing was delivered — the refusal is total, not partial");

    // The refusal consumed nothing: the mark is still there, so the default emission still
    // discharges it. A refusal that ate the sweep would be a lost delivery.
    (void)fx.g.propagate(fx.s);
    check(at_root.size() == 1, "the retained mark still flushes under the per-vertex default");
}

void test_selected_stream_is_refused() {
    std::printf("a selected STREAM refuses the fold (a LIST has no §B seat):\n");
    log_t at_root;
    auto on_root = [&at_root](const rope_t& v) { at_root.push_back(bytes_of(v)); };
    graph_t g;
    vertex_handle_t s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    vertex_handle_t st = g.register_vertex(path_t("/s/st"), role_t::STREAM);
    g.set_history_depth(st, 4);  // else the ring keeps one entry and there is no LIST to refuse
    check(g.subscribe(path_t("/s"), on_root).has_value(), "subtree subscriber at /s");
    check(g.assign(st, make_value(value_tlv({0x01}))).has_value(), "append one stream entry");
    check(g.assign(st, make_value(value_tlv({0x02}))).has_value(), "append a second stream entry");

    const auto folded = g.propagate(s, emission_mode_t::FOLD);
    check(!folded && folded.error() == status_t::TYPE_MISMATCH,
          "FOLD refuses: a since-flush LIST cannot ride a node that admits one VALUE");
    check(at_root.empty(), "nothing delivered on the refusal");
    (void)g.propagate(s);
    check(at_root.size() == 2, "PER_VERTEX still delivers both buffered stream entries");
}

void test_disjoint_subtrees_are_several_frames() {
    std::printf("disjoint subtrees are SEVERAL branch writes, never a container (§E):\n");
    log_t at_p;
    log_t at_q;
    auto on_p = [&at_p](const rope_t& v) { at_p.push_back(bytes_of(v)); };
    auto on_q = [&at_q](const rope_t& v) { at_q.push_back(bytes_of(v)); };
    graph_t g;
    vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    vertex_handle_t pl = g.register_vertex(path_t("/p/l"), role_t::STORED_VALUE);
    vertex_handle_t q = g.register_vertex(path_t("/q"), role_t::STORED_VALUE);
    vertex_handle_t ql = g.register_vertex(path_t("/q/l"), role_t::STORED_VALUE);
    check(g.subscribe(path_t("/p"), on_p).has_value(), "subscribe at /p");
    check(g.subscribe(path_t("/q"), on_q).has_value(), "subscribe at /q");
    check(g.assign(pl, make_value(value_tlv({0x11}))).has_value(), "assign /p/l");
    check(g.assign(ql, make_value(value_tlv({0x22}))).has_value(), "assign /q/l");

    check(g.propagate(p, emission_mode_t::FOLD).has_value(), "fold the /p subtree");
    check(g.propagate(q, emission_mode_t::FOLD).has_value(), "fold the /q subtree");
    check(at_p.size() == 1 && at_q.size() == 1, "one frame per subtree — two subtrees, two frames");
    check(at_p.size() == 1 && root_name_of(at_p[0]) == "p", "/p's frame is rooted at p");
    check(at_q.size() == 1 && root_name_of(at_q[0]) == "q", "/q's frame is rooted at q");
    check(at_p.size() == 1 && at_q.size() == 1 &&
              !contains(at_p[0], std::span<const std::byte>(at_q[0])),
          "neither frame contains the other — there is no container across subtrees");
}

}  // namespace

int main() {
    std::printf("libtracer propagate FOLD emission tests (RFC-0025 §4.1.2 clause 5):\n");
    test_default_emission_unchanged();
    test_fold_is_one_frame_with_the_same_leaf_truth();
    test_trailer_carrying_node_is_rejected();
    test_selected_stream_is_refused();
    test_disjoint_subtrees_are_several_frames();
    return tr::testing::summary("propagate_fold");
}
