/**
 * @file
 * @brief RFC-0027 car 3 — the mixed-element codec (`path_element.hpp`) and the origin-side
 *        label cache on `path_t` (§6.1), with §9's layering discipline asserted rather than
 *        assumed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Three things this file exists to catch, none of which any earlier test can:
 *
 * 1. **A foreign escape kind and a malformed label are different answers** (§12.5 erratum 1).
 *    Car 2's `path_label_at` answers `nullopt` to both, because a hop reading ONE element
 *    responds to both the same way. A hop reading a WHOLE body does not: it steps over the
 *    first by its declared length and refuses the address on the second. Folding them would
 *    let a length-only check read another kind's payload against the local table.
 * 2. **A label replaces a whole mount run** (amendment 6). The splice is vectored at one and
 *    at three segments, in the same test, so the two cannot silently diverge — which is the
 *    §12.5 requirement, transposed from the vector harness to the codec that feeds it.
 * 3. **§9's layering is structural.** Caching a labelled spelling must not move `key()`, must
 *    not make a labelled body a lookup key, and must never come out of `parse`. Those are
 *    three separate ways the "opaque to the application" rule could be lost, and a
 *    by-construction argument is not a test.
 *
 * NOTHING here mints: car 3 delivers the codec surface and the cache STRUCTURE, and the
 * trigger, the reply-leg rewrite, the deref and the `NOT_FOUND` answer are car 4's.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/packed_path.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/path_label.hpp"
#include "libtracer/path_ref.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using tr::wire::path_element_at;
using tr::wire::path_element_census;
using tr::wire::path_element_cursor_t;
using tr::wire::path_element_kind_t;
using tr::wire::path_element_t;
using tr::wire::path_label_t;

/** @brief A label a host could have minted — slot 5, generation 2 (§4.1). */
constexpr path_label_t kLabel{.index = 5, .generation = 2};

/** @brief Append the packed records for @p segs, in order. */
std::vector<std::byte> packed(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> out;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(out, s);
    return out;
}

/** @brief One escape record `00 <kind> <len> <payload>`, spelled by hand. */
std::vector<std::byte> escape(std::uint8_t kind, std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    (void)tr::wire::emit_path_escape(out, kind, payload);
    return out;
}

/** @brief Concatenate two packed bodies. */
std::vector<std::byte> operator+(std::vector<std::byte> a, const std::vector<std::byte>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/** @brief Classification of every record of a pure-string body, and the walk that ends. */
void literal_elements() {
    const std::vector<std::byte> body = packed({"net", "ws-client", "board-01"});
    path_element_cursor_t cur(body);

    const auto a = cur.next();
    check(a && a->kind == path_element_kind_t::SEGMENT && a->text() == "net" && a->at == 0 &&
              a->bytes == 4,
          "a literal record reads as a SEGMENT, with its own offset and width");
    const auto b = cur.next();
    check(b && b->text() == "ws-client" && b->at == 4, "the walk steps by the record's bytes");
    const auto c = cur.next();
    check(c && c->text() == "board-01", "and reaches the last record");
    check(!cur.next() && cur.done(), "the walk ends at the body's end, not one record past it");

    check(path_element_census(body) ==
              tr::wire::path_element_census_t{
                  .well_formed = true, .elements = 3, .segments = 3, .labels = 0, .foreign = 0},
          "a pure-string body censuses as three segments and no labels");
    check(path_element_census({}).well_formed && path_element_census({}).elements == 0,
          "an EMPTY body is the graph root: well-formed, zero elements");
}

/** @brief RFC-0027 §5.2 — name and label elements mix freely, in either order. */
void mixed_elements() {
    std::vector<std::byte> label_rec;
    check(tr::wire::emit_path_label(label_rec, kLabel), "the label element spells");
    check(label_rec.size() == tr::wire::kPathLabelRecordBytes, "…in 7 bytes (§5.3.2)");

    const std::vector<std::byte> label_first = label_rec + packed({"sensor", "temp"});
    const std::vector<std::byte> label_last = packed({"sensor", "temp"}) + label_rec;

    for (const auto* body : {&label_first, &label_last}) {
        const auto census = path_element_census(*body);
        check(census.well_formed && census.elements == 3 && census.segments == 2 &&
                  census.labels == 1,
              "a mixed body reads as two segments and one label, whatever the order (§5.2)");
    }

    const path_element_t head = path_element_at(label_first, 0);
    check(head.kind == path_element_kind_t::LABEL && head.label == kLabel &&
              head.escape_kind == tr::wire::kPackedEscapeKindLabel &&
              head.bytes == tr::wire::kPathLabelRecordBytes,
          "the label element decodes to the value that was spelled — index AND generation");

    const path_element_t tail = path_element_at(label_last, label_last.size() - label_rec.size());
    check(tail.kind == path_element_kind_t::LABEL && tail.label == kLabel,
          "position decides nothing: an element is read by its kind (§5.1)");

    check(!tr::wire::packed_path_valid_key(label_first) &&
              !tr::wire::packed_path_valid_key(label_last),
          "a PATH carrying a label is NEVER a path_lookup_key (amendment 5) — car 2's rule, "
          "re-asserted here because the codec is what would erode it");
}

/** @brief The two structural clauses, kept apart: a foreign kind SKIPS, a bad label REFUSES. */
void foreign_versus_malformed() {
    const std::array<std::byte, 2> payload{std::byte{0xAA}, std::byte{0xBB}};
    const std::vector<std::byte> foreign =
        packed({"net"}) + escape(0x17, payload) + packed({"temp"});
    const auto fc = path_element_census(foreign);
    check(fc.well_formed && fc.elements == 3 && fc.foreign == 1 && fc.labels == 0,
          "an escape kind this host does not own is FOREIGN — walked over by its declared "
          "length, never read as a label");
    check(path_element_at(foreign, 4).escape_kind == 0x17 &&
              path_element_at(foreign, 4).payload.size() == 2,
          "…and its payload is handed back intact for a relay, not interpreted");

    // kind 0x16 with a payload that is not exactly one label: `path-label/label-wrong-length`,
    // whose ruled answer is `tr::path::invalid` — a RESOLVER refusal of a malformed address.
    const std::array<std::byte, 3> shortpay{std::byte{1}, std::byte{0}, std::byte{0}};
    const std::vector<std::byte> wrong_len =
        packed({"net"}) + escape(tr::wire::kPackedEscapeKindLabel, shortpay);
    check(!path_element_census(wrong_len).well_formed,
          "a 0x16 record whose length is not 4 refuses the ADDRESS (§12.5 erratum 1 clause 2)");
    check(path_element_at(wrong_len, 4).kind == path_element_kind_t::MALFORMED,
          "…and refuses it as MALFORMED, not as an unknown kind to be skipped");

    // The reserved zero generation: STRUCTURALLY a label element (§12.5 erratum 1 fixes exactly
    // two clauses, the kind and the len, and this record satisfies both), carrying a value no
    // host ever minted. The refusal it earns is §7.2's NOT_FOUND-class one at the deref, with
    // the sender falling back to strings — not `tr::path::invalid`, which is the answer to a
    // malformed ADDRESS. Classifying it as malformed here would answer the wrong error code on
    // the wire, and a wire-surface divergence is an amendment's business, not a codec's.
    const std::array<std::byte, 4> zero_gen{std::byte{7}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::vector<std::byte> reserved = escape(tr::wire::kPackedEscapeKindLabel, zero_gen);
    const path_element_t reserved_el = path_element_at(reserved, 0);
    check(reserved_el.kind == path_element_kind_t::LABEL && !reserved_el.label.valid(),
          "generation 0 reads as a LABEL element carrying an invalid path label — the deref "
          "refuses it with NOT_FOUND, which is §7.2's answer for an unminted slot");
    check(path_element_census(reserved).well_formed && path_element_census(reserved).labels == 1,
          "…so it does NOT make the body malformed: a value-level refusal is not a structural one");

    // A ragged record: the length byte claims more than the body holds.
    const std::vector<std::byte> ragged{std::byte{9}, std::byte{'a'}};
    check(path_element_at(ragged, 0).kind == path_element_kind_t::MALFORMED &&
              path_element_at(ragged, 0).bytes == 0,
          "a record running past the body is MALFORMED with no width to step by");

    path_element_cursor_t cur(wrong_len);
    check(cur.next() && cur.next() && !cur.next(),
          "the walk returns a refusal exactly once and then ends — it never resynchronizes past "
          "one, which would be inventing the rest of somebody's route");
}

/** @brief Amendment 6 — one label covers a hop's whole local part, one segment or five. */
void splice_replaces_a_run() {
    const std::vector<std::byte> canonical =
        packed({"net", "ws-client", "board-01", "sensor", "temp"});

    // The three-segment mount run `net/ws-client/board-01` becomes ONE element.
    std::vector<std::byte> multi;
    check(tr::wire::emit_path_labelled(multi, canonical, 0, 3, kLabel),
          "a three-segment mount run splices to one label element (amendment 6)");
    std::vector<std::byte> expect_multi;
    check(tr::wire::emit_path_label(expect_multi, kLabel), "spell the expected label");
    expect_multi = expect_multi + packed({"sensor", "temp"});
    check(multi == expect_multi, "…byte for byte");
    const auto mc = path_element_census(multi);
    check(mc.elements == 3 && mc.labels == 1 && mc.segments == 2,
          "the spliced body is one label plus the residual, and walks cleanly");
    check(multi.size() == canonical.size() - 23 + tr::wire::kPathLabelRecordBytes,
          "the rewrite REPLACES bytes and never appends (§6.1) — 23 B of run for 7 B of label");

    // The one-segment case, in the same test so the two cannot diverge (§12.5).
    std::vector<std::byte> single;
    check(tr::wire::emit_path_labelled(single, canonical, 0, 1, kLabel),
          "a one-segment run mints a label covering one segment");
    check(path_element_census(single).elements == 5, "…and the other four records stand");

    // A run in the middle and at the tail: nothing about the splice is positional.
    std::vector<std::byte> middle;
    check(tr::wire::emit_path_labelled(middle, canonical, 1, 2, kLabel) &&
              path_element_at(middle, 4).kind == path_element_kind_t::LABEL,
          "a run in the middle splices where it stands");
    std::vector<std::byte> tail;
    check(tr::wire::emit_path_labelled(tail, canonical, 3, 2, kLabel) &&
              path_element_census(tail).elements == 4,
          "so does the terminus residual — the part a PATH_REF reaches only by replacing "
          "the whole address (§3.3)");

    // Every refusal appends NOTHING.
    for (const auto& [first, count] :
         std::array<std::pair<std::size_t, std::size_t>, 3>{{{5, 1}, {3, 3}, {0, 0}}}) {
        std::vector<std::byte> out{std::byte{0xEE}};
        check(
            !tr::wire::emit_path_labelled(out, canonical, first, count, kLabel) && out.size() == 1,
            "an out-of-range or empty run refuses, appending nothing");
    }
    std::vector<std::byte> out;
    check(!tr::wire::emit_path_labelled(out, canonical, 0, 3, path_label_t{}) && out.empty(),
          "an unmintable label (the reserved zero generation) never reaches the wire");
    check(!tr::wire::emit_path_labelled(out, multi, 0, 2, kLabel) && out.empty(),
          "a run holding an element that is already a label refuses — §5.3.3 with §6.1: what a "
          "label stands for is the mount RUN a hop strips, and that is a run of names");

    // The run bound is checked BEFORE any `first + count` is formed. Unguarded, both of these
    // wrap: the first appends a 7-byte label to a body it must not touch (violating §6.1's
    // replaces-never-appends), and the second emits `net/ws-client/board-01/<label>/…` — a
    // well-formed spelling of a DIFFERENT address, which is the mis-delivery class this design
    // closes by construction everywhere else.
    constexpr std::size_t kMax = static_cast<std::size_t>(-1);
    check(!tr::wire::emit_path_labelled(out, canonical, kMax, 1, kLabel) && out.empty(),
          "`first = SIZE_MAX, count = 1` refuses and appends nothing — the sum never wraps");
    check(!tr::wire::emit_path_labelled(out, canonical, kMax - 1, 3, kLabel) && out.empty(),
          "nor does `first = SIZE_MAX - 1, count = 3`, which wrapped to an in-run window that "
          "spliced a label into a route it was never given");
    check(!tr::wire::emit_path_labelled(out, canonical, 0, kMax, kLabel) && out.empty(),
          "a count past the body refuses on the byte bound, before the walk");
}

/** @brief RFC-0027 §6.1's origin-side cache, and §9's layering rule made structural. */
void origin_side_cache() {
    tr::graph::path_t p("/net/ws-client/board-01/sensor/temp");
    const std::vector<std::byte> canonical(p.key().begin(), p.key().end());
    check(!p.path_label().cached && p.path_label().body.empty(),
          "a path is unlabelled until a minted reply comes back — no allocation, nothing read");
    check(tr::wire::packed_path_valid_key(p.key()),
          "and `parse` never produces a labelled body: canonical keys stay pure-string (§9)");

    std::vector<std::byte> minted;
    check(tr::wire::emit_path_labelled(minted, canonical, 0, 3, kLabel), "a reply comes minted");
    check(p.cache_path_label(minted), "the net tier caches the spelling the reply carried");
    check(p.path_label().cached && p.path_label().body == minted, "…verbatim");

    check(std::ranges::equal(p.key(), canonical),
          "THE layering rule (§9): caching a label does not move the vertex-map key. The "
          "canonical bytes are what the label was minted FROM and what a refusal falls back "
          "to, so they are never discarded");
    check(!tr::wire::packed_path_valid_key(p.path_label().body),
          "and the labelled spelling is not a lookup key, whatever a caller does with it");

    tr::graph::path_t copy = p;
    check(copy.path_label().body == minted,
          "a path is a VALUE: copying it copies the slot, and neither copy holds a handle into "
          "the tier that filled it");

    p.clear_path_label();
    check(!p.path_label().cached && p.path_label().body.empty(),
          "§7.2's recovery is forgetting the bytes — no withdraw, no unbind, no lease, no TTL");

    // Refusals: each leaves the path exactly as it was.
    check(!p.cache_path_label(canonical) && !p.path_label().cached,
          "a body with NO label element is refused — `key()` already holds that spelling");
    check(!p.cache_path_label({}) && !p.path_label().cached, "an empty body is not a minted reply");
    const std::array<std::byte, 3> shortpay{std::byte{1}, std::byte{0}, std::byte{0}};
    check(!p.cache_path_label(escape(tr::wire::kPackedEscapeKindLabel, shortpay)),
          "a malformed label element is refused here too: caching one would hand the next "
          "operation an address the far hop must refuse");
    std::vector<std::byte> oversize;
    while (oversize.size() <= tr::graph::kMaxPathBytes)
        (void)tr::wire::emit_path_segment(oversize, "padpadpadpadpad");
    oversize = oversize + minted;
    check(!p.cache_path_label(oversize), "and the canonical byte bound binds both spellings");
}

/**
 * @brief RFC-0027 §11.2 — the ONE arm of the recommendation this car implements, and the
 *        deliberate asymmetry beside it.
 *
 * §11.2 is *"Recommended, pending the §12.4 measurement"* and reads SHOULD NOT twice, leaving
 * the per-route choice to a host implementing both. So the refusal lands only on the surface
 * this car introduces — `cache_path_label`, where stating it costs nothing because no caller
 * exists yet — and `bind` keeps RFC-0024's shipped behaviour exactly. Tightening a shipped API
 * on a SHOULD, before the measurement that would justify it, is a bigger change than the
 * recommendation asks for.
 */
void one_compression_per_address() {
    const std::array<tr::wire::path_ref_element_t, 2> refs{
        tr::wire::path_ref_element_t{.index = 1, .generation = 1},
        tr::wire::path_ref_element_t{.index = 2, .generation = 1}};

    tr::graph::path_t bound_first("/net/ws-client/board-01/sensor/temp");
    const std::vector<std::byte> canonical(bound_first.key().begin(), bound_first.key().end());
    std::vector<std::byte> minted;
    check(tr::wire::emit_path_labelled(minted, canonical, 0, 3, kLabel), "a minted spelling");

    check(bound_first.bind(refs), "a path binds a PATH_REF as it always has");
    check(!bound_first.cache_path_label(minted) && !bound_first.path_label().cached,
          "and a bound path then refuses a path-label spelling on top of it (§11.2)");

    tr::graph::path_t labelled_first("/net/ws-client/board-01/sensor/temp");
    check(labelled_first.cache_path_label(minted), "the other order: path labels first");
    check(labelled_first.bind(refs) && labelled_first.binding().bound,
          "…and `bind` is UNCHANGED — RFC-0024's shipped surface is not tightened on a SHOULD "
          "that is itself pending §12.4's measurement");

    labelled_first.clear_path_label();
    check(!labelled_first.path_label().cached && labelled_first.bind(refs),
          "dropping the path-label form frees the other arm — the refusal is a state check, "
          "not a latch");
}

}  // namespace

/** @brief Runs every RFC-0027 car-3 check; non-zero exit ⇒ some check failed. */
int main() {
    literal_elements();
    mixed_elements();
    foreign_versus_malformed();
    splice_replaces_a_run();
    origin_side_cache();
    one_compression_per_address();
    return tr::testing::summary("path_label_codec");
}
