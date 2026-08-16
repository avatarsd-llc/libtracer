/**
 * @file
 * @brief The RFC-0027 path-label value, its §5.3 ELEMENT, and the mint table — codec, element
 *        grammar, the §12.5 vectors, mint, lookup, retire, SATURATE-AND-RETIRE, and §8.3.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Three halves, and the last is the one that needs a test at all.
 *
 * The codec half pins §4.1's 32-bit value: a u16 slot index and a u16 generation, one
 * little-endian u32, with `0` reserved as "no label".
 *
 * The element half pins §5.3, now CLOSED by amendments 4-6: the label element is
 * RFC-0018 §8's escape record at `kind = 0x16`, 7 bytes, mixable with name records in
 * any order, one element per hop's whole local part, and NEVER admissible as a
 * `path_lookup_key`. `conformance_vectors` builds each §12.5 `path-label/` vector from
 * the emitter and asserts it byte-for-byte, so the vectors pin the spelling rather than
 * merely accompanying it (ADR-0028: the C++ core is golden).
 *
 * The table half pins §§7-8 and, above all, **§4.3.1**: the generation saturates and
 * the slot RETIRES PERMANENTLY, never wraps. That rule is invisible on the wire — no
 * byte, no flag and no frame changes — so a regression would pass every conformance
 * vector, every round-trip and every fan-out bench, and would surface only as #603's
 * mis-delivery class re-opening on a peer holding a label across 65 535 reuses of one
 * slot. The acceptance train calls this out as needing its own test for exactly that
 * reason, and `saturate_and_retire` below is it.
 */

#include "libtracer/path_label.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/packed_path.hpp"
#include "libtracer/path_label_table.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "test_support.hpp"

namespace {

using tr::net::path_label_table_t;
using tr::net::path_label_target_t;
using tr::net::peer_handle_t;
using tr::testing::check;
using tr::wire::path_label_t;

/** @brief A peer identity for the table's per-peer accounting — any valid handle will do. */
constexpr peer_handle_t kPeerA{7, 1};
/** @brief A second, distinct peer — the same slot index means nothing to it (§4.1). */
constexpr peer_handle_t kPeerB{9, 1};

/** @brief A stand-in resolution: what a label aliases is opaque to the table. */
constexpr path_label_target_t kTarget{.index = 42, .generation = 3};

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

/** @brief §4.1's value: the 16/16 split, the reserved zero, and the u32 round trip. */
void value_shape() {
    const path_label_t l{.index = 0x1234, .generation = 0xABCD};
    check(l.bits() == 0xABCD1234U, "a path label's u32 is (generation << 16) | index");
    check(tr::wire::path_label_from_bits(l.bits()) == l, "bits() round-trips through from_bits");
    check(l.valid(), "a non-zero generation names a minted slot");
    check(!path_label_t{}.valid(), "generation 0 is reserved 'no label' and never validates");
    check(tr::wire::kPathLabelBodyBytes == 4, "a path label is 4 bytes (RFC-0027 §4.1, §3.3)");
    check(tr::wire::kPathLabelSlotSpace == 65536, "a u16 index names 65 536 slots");
}

/** @brief The little-endian body — §5.3's four bytes, in the order every field takes. */
void byte_layout() {
    std::vector<std::byte> body(tr::wire::kPathLabelBodyBytes);
    tr::wire::path_label_store(body, path_label_t{.index = 0x1234, .generation = 0xABCD});
    check(body[0] == std::byte{0x34} && body[1] == std::byte{0x12} && body[2] == std::byte{0xCD} &&
              body[3] == std::byte{0xAB},
          "the label body is one little-endian u32: index low half, generation high half");
    check(tr::wire::path_label_load(body) == path_label_t{.index = 0x1234, .generation = 0xABCD},
          "path_label_load is path_label_store's inverse");

    // §5.3.2's two structural clauses, kept apart: the kind names the element, the length
    // says it is exactly one label. Two clauses, two vectors (RFC-0024 §9.4).
    check(tr::wire::path_label_record_valid(0x16, 4), "kind 0x16 with a 4-byte payload is a label");
    check(!tr::wire::path_label_record_valid(0x17, 4),
          "a foreign kind is NOT a label, whatever its length (label-foreign-kind)");
    check(
        !tr::wire::path_label_record_valid(0x16, 3) && !tr::wire::path_label_record_valid(0x16, 8),
        "a payload that is not exactly one label is rejected (label-wrong-length)");
    check(tr::wire::kPathLabelRecordBytes == 7,
          "a label ELEMENT is 7 bytes: 00 <kind> <len> + the 4-byte value (§5.3.2)");
}

/**
 * @brief §5.3's ruled element — RFC-0018's escape at `kind = 0x16`, emitted and read back.
 *
 * The three amendments in one place: the element is its own self-describing record (4), it is
 * the 7-byte escape and NOT the 8-byte `PATH_LABEL` TLV child (5), and it is the same 7 bytes
 * whether the local part it stands for is one segment or five (6).
 */
void element_grammar() {
    const path_label_t label{.index = 1, .generation = 2};
    std::vector<std::byte> body;
    check(tr::wire::emit_path_label(body, label), "a valid label emits its element");
    check(body.size() == tr::wire::kPathLabelRecordBytes, "the element is 7 bytes");
    check(body[0] == std::byte{0x00} && body[1] == std::byte{0x16} && body[2] == std::byte{0x04},
          "the framing is the RFC-0018 escape: len 0, kind 0x16, payload length 4");
    check(tr::wire::path_label_at(body, 0) == label, "the element reads back as the label emitted");

    // §4.1's reserved generation never reaches the wire, not even through a default-constructed
    // struct — the emitter refuses it rather than spelling a label nobody minted.
    std::vector<std::byte> refused;
    check(!tr::wire::emit_path_label(refused, path_label_t{}) && refused.empty(),
          "generation 0 is refused, appending NOTHING");

    // §5.2: names and labels mix in any order, and a label is read by its KIND, never by its
    // position — which is why the second element below is found by walking, not by counting.
    std::vector<std::byte> mixed;
    (void)tr::wire::emit_path_segment(mixed, "sensor");
    check(tr::wire::emit_path_label(mixed, label), "a label follows a name");
    (void)tr::wire::emit_path_segment(mixed, "temp");
    const path_label_t saturated{.index = 0x0102, .generation = tr::wire::kPathLabelMaxGeneration};
    check(tr::wire::emit_path_label(mixed, saturated), "a name follows a label");
    std::size_t at = 0;
    std::vector<path_label_t> seen;
    while (at < mixed.size()) {
        const std::size_t span = tr::wire::packed_record_span(mixed, at);
        check(span != 0, "every record in a mixed body is well framed");
        if (const auto l = tr::wire::path_label_at(mixed, at)) seen.push_back(*l);
        at += span;
    }
    check(seen.size() == 2 && seen[0] == label && seen[1] == saturated,
          "a mixed body walks with `p += span` and yields exactly its label elements");
    check(saturated.valid(),
          "generation 0xFFFF is a USABLE label — saturation forbids the next MINT, not this use");

    // §5.3 sub-question 3, ruled NOT admissible: a labelled PATH is never a canonical key,
    // while its pure-string twin still is. This is what keeps RFC-0018 §5's injectivity and
    // §5.1's byte-prefix-implies-ancestor invariant true once labels exist.
    check(!tr::wire::packed_path_valid_key(mixed), "a labelled PATH is NOT a path_lookup_key");
    std::vector<std::byte> strings;
    (void)tr::wire::emit_path_segment(strings, "sensor");
    (void)tr::wire::emit_path_segment(strings, "temp");
    check(tr::wire::packed_path_valid_key(strings), "its pure-string twin still is");

    // A foreign kind is somebody else's element: skippable by length, and NEVER read as a
    // label — the mis-delivery a length-only check would cause (label-foreign-kind).
    std::vector<std::byte> foreign;
    const std::array<std::byte, 4> four{std::byte{1}, std::byte{0}, std::byte{2}, std::byte{0}};
    check(tr::wire::emit_path_escape(foreign, 0x17, four), "an unassigned kind still frames");
    check(tr::wire::packed_record_span(foreign, 0) == 7, "and is skippable by its declared length");
    check(!tr::wire::path_label_at(foreign, 0).has_value(),
          "a foreign kind carrying four bytes is NOT read as a label");

    // A short payload is a malformed address, not a label read short (label-wrong-length).
    std::vector<std::byte> short_body;
    const std::array<std::byte, 3> three{std::byte{1}, std::byte{0}, std::byte{2}};
    check(tr::wire::emit_path_escape(short_body, tr::wire::kPackedEscapeKindLabel, three),
          "a 3-byte payload at kind 0x16 is well FRAMED");
    check(tr::wire::packed_record_span(short_body, 0) == 6, "so a non-implementing hop skips it");
    check(!tr::wire::path_label_at(short_body, 0).has_value(),
          "but a hop that implements labels reads NOTHING from it");
    check(!tr::wire::path_label_at(strings, 0).has_value(), "a literal segment is not a label");
}

/**
 * @brief §12.5's vectors, byte-exact against this emitter — the C++ core is golden (ADR-0028).
 *
 * Building the bytes here rather than asserting a hex literal is what makes the vectors a PIN:
 * a change to the element spelling reddens this test, and a re-blessing that does not re-run
 * the emitter cannot pass it.
 */
void conformance_vectors() {
    const auto path_frame = [](std::span<const std::byte> body) {
        std::vector<std::byte> out;
        tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
        return out;
    };

    std::vector<std::byte> one;
    check(tr::wire::emit_path_label(one, path_label_t{.index = 1, .generation = 2}),
          "label-roundtrip's element emits");
    check(same(path_frame(one), vector_bytes("path-label/label-roundtrip")),
          "path-label/label-roundtrip is byte-exact against the emitter");

    std::vector<std::byte> mixed;
    (void)tr::wire::emit_path_segment(mixed, "sensor");
    (void)tr::wire::emit_path_label(mixed, path_label_t{.index = 1, .generation = 2});
    (void)tr::wire::emit_path_segment(mixed, "temp");
    (void)tr::wire::emit_path_label(
        mixed, path_label_t{.index = 0x0102, .generation = tr::wire::kPathLabelMaxGeneration});
    check(same(path_frame(mixed), vector_bytes("path-label/label-mixed")),
          "path-label/label-mixed is byte-exact against the emitter");

    // §5.3.3: the run /net/downlink/a is 15 packed bytes and ONE 7-byte element — the element
    // is the same width whatever the run's depth, which is the amendment's whole content.
    std::vector<std::byte> run;
    for (const std::string_view s : {"net", "downlink", "a"})
        (void)tr::wire::emit_path_segment(run, s);
    check(run.size() == 15, "the three-segment mount run packs to 15 bytes");
    std::vector<std::byte> multi;
    (void)tr::wire::emit_path_label(multi, path_label_t{.index = 3, .generation = 1});
    check(multi.size() == tr::wire::kPathLabelRecordBytes,
          "and compacts to one 7-byte element, not one per segment (§5.3.3)");
    for (const std::string_view s : {"sensor", "temp"}) (void)tr::wire::emit_path_segment(multi, s);
    check(same(path_frame(multi), vector_bytes("path-label/label-multi-segment")),
          "path-label/label-multi-segment is byte-exact against the emitter");

    std::vector<std::byte> wrong_len;
    (void)tr::wire::emit_path_segment(wrong_len, "sensor");
    const std::array<std::byte, 3> three{std::byte{1}, std::byte{0}, std::byte{2}};
    (void)tr::wire::emit_path_escape(wrong_len, tr::wire::kPackedEscapeKindLabel, three);
    check(same(path_frame(wrong_len), vector_bytes("path-label/label-wrong-length")),
          "path-label/label-wrong-length is byte-exact against the emitter");
    check(!tr::wire::path_label_at(wrong_len, 7).has_value() &&
              !tr::wire::packed_path_valid_key(wrong_len),
          "and it is neither a label nor a key");

    std::vector<std::byte> foreign;
    (void)tr::wire::emit_path_segment(foreign, "sensor");
    const std::array<std::byte, 2> two{std::byte{0xAA}, std::byte{0xBB}};
    (void)tr::wire::emit_path_escape(foreign, 0x17, two);
    check(same(path_frame(foreign), vector_bytes("path-label/label-foreign-kind")),
          "path-label/label-foreign-kind is byte-exact against the emitter");
    check(!tr::wire::path_label_at(foreign, 7).has_value() &&
              tr::wire::packed_record_span(foreign, 7) == 5,
          "and it is skipped by length, never read as a label");
}

/** @brief Mint then look up — the whole point of the table, and the peer scoping around it. */
void mint_and_lookup() {
    path_label_table_t t(std::pmr::get_default_resource(), 8);
    const auto label = t.mint(kPeerA, kTarget);
    check(label.has_value() && label->valid(), "a mint under capacity yields a valid label");
    check(t.live_count() == 1 && t.live_count_for(kPeerA) == 1, "the mint is counted, once");
    check(t.lookup(kPeerA, *label) == kTarget, "lookup returns the resolution the label aliases");

    // §4.1's scope rule: a label means something only to the peer it was minted for. A leaked
    // label buys an attacker one NOT_FOUND and no state (§8.4).
    check(!t.lookup(kPeerB, *label).has_value(), "a label presented by another peer is refused");
    check(!t.lookup(kPeerA,
                    path_label_t{.index = label->index,
                                 .generation = static_cast<std::uint16_t>(label->generation + 1)})
               .has_value(),
          "a generation mismatch is refused (§7.2)");
    check(!t.lookup(kPeerA, path_label_t{.index = 99, .generation = 1}).has_value(),
          "an out-of-range index is refused, never read (§4.1's bounds check)");
    check(!t.mint(peer_handle_t{}, kTarget).has_value(),
          "an invalid peer handle mints nothing — an unowned label has no scope");

    // Distinct peers get distinct slots, and each is scoped to its own owner.
    const auto b = t.mint(kPeerB, path_label_target_t{.index = 5, .generation = 1});
    check(b.has_value() && b->index != label->index, "a second peer's mint takes its own slot");
    check(t.live_count() == 2 && t.live_count_for(kPeerB) == 1, "each peer is counted separately");
}

/** @brief §7.1's departure bump: the label stops validating, and the slot comes back. */
void release_bumps_and_reuses() {
    path_label_table_t t(std::pmr::get_default_resource(), 4);
    const auto first = t.mint(kPeerA, kTarget).value();
    check(t.release(first), "a live label releases");
    check(t.live_count() == 0 && t.live_count_for(kPeerA) == 0, "the census drops with it");
    check(!t.lookup(kPeerA, first).has_value(), "the released label no longer validates (§7.1)");
    check(!t.release(first), "a second release of the same label is a no-op, not a double free");

    const auto second = t.mint(kPeerA, path_label_target_t{.index = 1, .generation = 1}).value();
    check(second.index == first.index, "the freed slot is reused");
    check(second.generation != first.generation,
          "the reused slot carries a fresh generation, so the old label stays refused");
    check(!t.lookup(kPeerA, first).has_value(),
          "the PREDECESSOR's label does not validate against its successor — the misroute class");
    check(t.lookup(kPeerA, second) == path_label_target_t{.index = 1, .generation = 1},
          "the successor resolves to its own target");

    // A stale release must not retire a successor's live label.
    check(!t.release(first), "a stale release is ignored");
    check(t.lookup(kPeerA, second).has_value(), "the live successor survives the stale release");

    // The peer-departure sweep (§7.1), on the identity the receiver seam hands down.
    const auto other = t.mint(kPeerB, kTarget).value();
    check(t.release_peer(kPeerA) == 1, "release_peer releases that peer's labels only");
    check(t.lookup(kPeerB, other).has_value(), "another peer's labels are untouched");
    check(t.live_count() == 1, "and the census reflects exactly that");
}

/**
 * @brief §4.3.1 — the generation SATURATES and the slot RETIRES PERMANENTLY, never wraps.
 *
 * A one-slot table is cycled until minting refuses. What must hold at the end: the slot's
 * generation never returned to a value a peer could still be holding (so no stale label ever
 * validates falsely — #603's class, closed by construction), the slot is gone from the mintable
 * set for good, and the degrade is the ordinary §8.3 refusal rather than an error.
 */
void saturate_and_retire() {
    path_label_table_t t(std::pmr::get_default_resource(), 1);
    const auto very_first = t.mint(kPeerA, kTarget).value();
    check(very_first.generation == 1, "a fresh slot mints at generation 1, never 0");
    check(t.release(very_first), "…and releases");

    std::uint16_t highest = very_first.generation;
    std::size_t cycles = 1;
    // The monotonicity of 65 533 mints is accumulated and asserted ONCE: a check per cycle
    // would print 65 533 lines to say one thing.
    bool monotonic = true;
    for (;;) {
        const auto l = t.mint(kPeerA, kTarget);
        if (!l.has_value()) break;
        monotonic = monotonic && l->generation > highest;
        highest = l->generation;
        static_cast<void>(t.release(*l));
        ++cycles;
    }

    check(monotonic, "a generation only ever moves forward across every reuse (§7.1)");
    check(cycles == tr::wire::kPathLabelMaxGeneration - 1,
          "the slot serves generations 1..65534 and then stops — 65 534 mints, not 65 536");
    check(highest == tr::wire::kPathLabelMaxGeneration - 1,
          "the last label minted carries the generation below the saturation point");
    check(t.retired_slots() == 1, "the saturated slot is counted as retired");
    check(!t.mint(kPeerA, kTarget).has_value(),
          "a retired slot is never minted into again — not after reclamation, not ever");
    check(!t.mint(kPeerB, kTarget).has_value(), "…and not for a different peer either");
    check(t.release_peer(kPeerA) == 0, "nothing is live to release");
    check(!t.mint(kPeerA, kTarget).has_value(),
          "…and emptying the table does not revive the retired slot (§4.3.1)");
    check(t.refused_mints() >= 4, "every refusal is counted, so a silent bound is impossible");
    check(!t.lookup(kPeerA, path_label_t{.index = 0, .generation = highest}).has_value(),
          "the last label a peer could still hold does not validate against a retired slot");
}

/** @brief §8.3 — the per-peer ceiling and the table capacity both REFUSE, never evict. */
void ceiling_and_capacity() {
    // Per-peer ceiling: two peers, a table wide enough for both, a ceiling below it.
    path_label_table_t t(std::pmr::get_default_resource(), 8, 2);
    check(t.capacity() == 8 && t.max_per_peer() == 2, "capacity and ceiling are what was injected");
    const auto a1 = t.mint(kPeerA, kTarget).value();
    const auto a2 = t.mint(kPeerA, kTarget).value();
    check(!t.mint(kPeerA, kTarget).has_value(), "a peer at its ceiling is refused a new mint");
    check(t.refused_mints() == 1, "the refusal is counted");
    check(t.lookup(kPeerA, a1).has_value() && t.lookup(kPeerA, a2).has_value(),
          "and NOTHING was evicted — a live label is never reclaimed by pressure (§8.3)");
    check(t.mint(kPeerB, kTarget).has_value(),
          "one peer's ceiling does not consume another peer's budget");
    check(t.release(a1) && t.mint(kPeerA, kTarget).has_value(),
          "releasing under the ceiling makes room again");

    // Table capacity: one peer, no ceiling, and the injected capacity is the bound.
    path_label_table_t small(std::pmr::get_default_resource(), 2);
    const auto s1 = small.mint(kPeerA, kTarget).value();
    const auto s2 = small.mint(kPeerA, kTarget).value();
    check(s1.index != s2.index, "two live labels never share a slot");
    check(!small.mint(kPeerA, kTarget).has_value(), "a full table refuses a new mint");
    check(small.lookup(kPeerA, s1).has_value() && small.lookup(kPeerA, s2).has_value(),
          "the full table's live labels are untouched by the refusal");

    // The default: a host that mints nothing is conformant, and it is the safe default (§6.3).
    path_label_table_t none;
    check(none.capacity() == 0 && !none.mint(kPeerA, kTarget).has_value(),
          "an unsized table mints nothing — the string path, which always works");

    // The capacity clamp: a 16-bit index cannot name more than 65 536 slots.
    path_label_table_t huge(std::pmr::get_default_resource(), 1'000'000);
    check(huge.capacity() == tr::wire::kPathLabelSlotSpace,
          "an over-sized capacity is clamped to the index space, not trusted");
}

}  // namespace

/** @brief Runs every RFC-0027 label/mint-table check; non-zero exit ⇒ some check failed. */
int main() {
    value_shape();
    byte_layout();
    element_grammar();
    conformance_vectors();
    mint_and_lookup();
    release_bumps_and_reuses();
    saturate_and_retire();
    ceiling_and_capacity();
    return tr::testing::summary("path_label");
}
