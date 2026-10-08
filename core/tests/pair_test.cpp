/**
 * @file
 * @brief Sentinels for the shared PAIR header (#1700): one type, its little-endian load and
 *        store, the three aliases spelled over it and the one distinct type derived from it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Each type gets a sentinel that fails the build or the run if it drifts back into a struct
 * of its own: the vertex slot, the RFC-0024 path-ref element and the RFC-0029 PAIR element
 * ARE `tr::wire::pair_t`; the peer handle is the same eight bytes but stays a distinct type
 * that a vertex PAIR does not convert to.
 */
#include "libtracer/pair.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "libtracer/path_pair.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/peer_handle.hpp"
#include "libtracer/vertex_handle.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using tr::wire::pair_t;

// The three aliases: one struct, so a value moves between the slot, the RFC-0024 element and
// the RFC-0029 element without a conversion.
static_assert(std::is_same_v<tr::graph::vertex_slot_t, pair_t>, "vertex_slot_t is the PAIR");
static_assert(std::is_same_v<tr::wire::path_ref_element_t, pair_t>,
              "path_ref_element_t is the PAIR");
static_assert(std::is_same_v<tr::wire::path_pair_t, pair_t>, "path_pair_t is the PAIR");
static_assert(tr::wire::kPathRefElementBytes == tr::wire::kPairBytes);
static_assert(tr::wire::kPathPairBodyBytes == tr::wire::kPairBytes);

// The distinct type: same size and layout, derived from the PAIR, never converted TO.
static_assert(!std::is_same_v<tr::net::peer_handle_t, pair_t>, "a peer handle is its own type");
static_assert(std::is_base_of_v<pair_t, tr::net::peer_handle_t>);
static_assert(sizeof(tr::net::peer_handle_t) == sizeof(pair_t));
static_assert(!std::is_convertible_v<pair_t, tr::net::peer_handle_t>);

/** @brief The wire form: index then generation, each u32 little-endian. */
void wire_form() {
    constexpr pair_t p{.index = 0x04030201u, .generation = 0x08070605u};
    std::array<std::byte, tr::wire::kPairBytes> out{};
    tr::wire::pair_store_le(out, p);
    bool le = true;
    for (std::size_t i = 0; i < out.size(); ++i) le = le && out[i] == static_cast<std::byte>(i + 1);
    check(le, "pair_store_le writes index then generation, both little-endian");
    check(tr::wire::pair_load_le(out) == p, "pair_load_le inverts pair_store_le");
}

/** @brief Each alias reads and writes through the same load and store. */
void aliases() {
    constexpr pair_t p{.index = 7, .generation = 3};
    std::array<std::byte, 2 * tr::wire::kPathRefElementBytes> ref{};
    tr::wire::path_ref_store_element(std::span<std::byte>(ref).subspan(8), p);
    check(tr::wire::path_ref_element_at(ref, 1) == p, "the path-ref element round-trips a PAIR");

    std::array<std::byte, tr::wire::kPathPairRecordBytes> rec{};
    tr::wire::path_pair_store(rec, p);
    check(tr::wire::path_pair_at(rec, 0) == p, "the PAIR element round-trips a PAIR");
    check(tr::wire::pair_load_le(std::span<const std::byte>(rec).subspan(3)) == p,
          "and its payload is the shared wire form, after the 3-byte escape header");

    const tr::graph::vertex_slot_t slot = p;
    check(slot == p, "a vertex slot IS the pair, no conversion");
}

/** @brief The peer handle keeps its own meaning over the same bytes. */
void peer_handle() {
    constexpr tr::net::peer_handle_t h{7, 3};
    check(h.valid() && !tr::net::peer_handle_t{}.valid(), "generation 0 still means 'no peer'");
    check(h.bits() == ((std::uint64_t{3} << 32) | 7), "bits() is generation:index");
    check(static_cast<const pair_t&>(h) == pair_t{.index = 7, .generation = 3},
          "and it is the PAIR's layout underneath");
    check(tr::net::kSolePeerHandle.valid(), "the sole-peer constant is still a valid handle");
}

}  // namespace

int main() {
    wire_form();
    aliases();
    peer_handle();
    return tr::testing::summary("pair");
}
