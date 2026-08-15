/**
 * @file
 * @brief The RFC-0027 path-label value and mint table — codec, mint, lookup, retire,
 *        SATURATE-AND-RETIRE, and the §8.3 bounds.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two halves, and the second is the one that needs a test at all.
 *
 * The codec half pins §4.1's 32-bit value: a u16 slot index and a u16 generation, one
 * little-endian u32, with `0` reserved as "no label". §5.3's element FRAMING is
 * deferred pending the §12.5 conformance vectors, so nothing here asserts a type code
 * or a `PATH` child — only the four bytes the ruling already fixed, and the leading
 * candidate's structural predicate.
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

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/path_label_table.hpp"
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

    // §5.3's leading-candidate structural rules, PL/LL/length — the shape the §12.5 vectors
    // will confirm or replace. Nothing here is wired into the grammar yet, by design.
    check(tr::wire::path_label_body_valid(false, false, 4), "a 4-byte PL=0/LL=0 body is the shape");
    check(!tr::wire::path_label_body_valid(true, false, 4), "PL=1 on a scalar body is rejected");
    check(!tr::wire::path_label_body_valid(false, true, 4), "LL=1 under a 4-byte body is rejected");
    check(!tr::wire::path_label_body_valid(false, false, 3) &&
              !tr::wire::path_label_body_valid(false, false, 8),
          "a body that is not exactly one label is rejected (label-wrong-length)");
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
    mint_and_lookup();
    release_bumps_and_reuses();
    saturate_and_retire();
    ceiling_and_capacity();
    return tr::testing::summary("path_label");
}
