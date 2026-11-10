/**
 * @file
 * @brief #2672 (ruling 49) — a parent's child list keeps each child's name prefix beside its
 *        pointer, and a lookup that ties on the prefix still finds the right child.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The sorted child list compares a pointer-wide prefix of the record first and reads a child's
 * name only on a tie. The names below are chosen so that some siblings tie on a 4 B prefix and
 * on an 8 B one (a long shared stem of one length), some are told apart by the prefix alone
 * (short names), and some are separated only past the 32-bit prefix (seven bytes), so both
 * widths run both arms. They register in reverse order, so every insert lands mid-list.
 *
 * - (a) every name resolves, and a name that sits between two siblings, shares their stem,
 *       or extends one of them resolves to nothing.
 * - (b) the `:children` listing is in record order: the prefix does not reorder the list.
 * - (c) retiring the parent reaches every child (the sibling ascent re-finds each one by the
 *       same search), and a revived child is found again in its old place.
 * - (d) raw records, which the string doors cannot spell: bytes 0x00 and 0xFF in the head and
 *       past a tied prefix (the compare is unsigned), and a ragged record, whose length byte
 *       overruns it, tying with a well-framed one of a different length.
 */

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/tlv.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::testing::check;

/** @brief Siblings under `/p`, in their record order (`[u8 len][bytes]`: length first). */
constexpr std::string_view kSorted[] = {
    "a",
    "b",
    "c",
    "ab",
    "ba",
    "abc",
    "abd",
    "abcdefg",
    "abcdefh",
    "sensor_temp_a",
    "sensor_temp_b",
    "sensor_temp_c",
};

/** @brief Names no child holds: between siblings, on a shared stem, or extending one. */
constexpr std::string_view kAbsent[] = {
    "aa", "abe", "abcdefgg", "abcdefgh", "sensor_temp_", "sensor_temp_ab", "sensor_temp_d", "d",
};

std::string child(std::string_view name) { return "/p/" + std::string(name); }

bool found(const graph_t& g, std::string_view name) {
    return g.find(path_t(child(name)).key()).has_value();
}

/** @brief Where the member NAME TLV for @p name sits in @p listing, or npos. */
std::size_t member_at(std::span<const std::byte> listing, std::string_view name) {
    std::vector<std::byte> tlv{std::byte{static_cast<unsigned char>(tr::wire::type_t::NAME)},
                               std::byte{0}, std::byte{static_cast<unsigned char>(name.size())},
                               std::byte{0}};
    for (const char c : name) tlv.push_back(static_cast<std::byte>(c));
    const auto it = std::search(listing.begin(), listing.end(), tlv.begin(), tlv.end());
    return it == listing.end() ? std::string::npos : static_cast<std::size_t>(it - listing.begin());
}

void check_order(graph_t& g, std::string_view what) {
    const auto listed = g.read(path_t("/p:children"));
    check(listed.has_value(), "the :children listing reads");
    if (!listed) return;
    const auto bytes = (*listed)->flatten();
    std::size_t last = 0;
    bool ordered = true;
    for (const std::string_view n : kSorted) {
        const std::size_t at = member_at(bytes.bytes(), n);
        ordered = ordered && at != std::string::npos && (last == 0 || at > last);
        last = at;
    }
    check(ordered, what);
}

void test_prefix_ties_and_order() {
    graph_t g;
    bool registered = g.try_register_vertex(path_t("/p"), role_t::STORED_VALUE).has_value();
    std::vector<std::optional<tr::graph::vertex_handle_t>> first(std::size(kSorted));
    for (std::size_t i = std::size(kSorted); i-- > 0;) {
        const auto h = g.try_register_vertex(path_t(child(kSorted[i])), role_t::STORED_VALUE);
        registered = h.has_value() && registered;
        if (h) first[i] = *h;
    }
    check(registered, "every sibling registers, inserted in reverse record order");

    bool all = true;
    for (const std::string_view n : kSorted) all = found(g, n) && all;
    check(all, "(a) every sibling resolves, prefix ties included");
    bool none = true;
    for (const std::string_view n : kAbsent) none = !found(g, n) && none;
    check(none, "(a) a name between, on the stem of, or past a sibling resolves to nothing");

    check_order(g, "(b) the :children listing is in record order");

    const auto parent = g.find(path_t("/p").key());
    check(parent.has_value() && g.retire(*parent).has_value(), "(c) the parent retires");
    bool gone = true;
    for (const std::string_view n : kSorted) gone = !found(g, n) && gone;
    check(gone, "(c) retiring the parent reaches every child, prefix ties included");

    check(
        g.try_register_vertex(path_t("/p"), role_t::STORED_VALUE).has_value() &&
            g.try_register_vertex(path_t(child("sensor_temp_b")), role_t::STORED_VALUE).has_value(),
        "(c) a tied child revives");
    check(found(g, "sensor_temp_b") && !found(g, "sensor_temp_a") && !found(g, "sensor_temp_c"),
          "(c) the revived child is found and its retired twins stay retired");
    bool in_place = true;
    for (std::size_t i = 0; i < std::size(kSorted); ++i) {
        if (!found(g, kSorted[i]))
            (void)g.try_register_vertex(path_t(child(kSorted[i])), role_t::STORED_VALUE);
        const auto h = g.find(path_t(child(kSorted[i])).key());
        in_place = h.has_value() && first[i].has_value() && *h == *first[i] && in_place;
    }
    check(in_place, "(c) every sibling revives in place, on the vertex it had");
    check_order(g, "(c) revived siblings list in the same record order");
}

/** @brief The raw record bytes @p head, for a key no string door can spell. */
std::vector<std::byte> raw(std::initializer_list<unsigned> head) {
    std::vector<std::byte> r;
    for (const unsigned b : head) r.push_back(static_cast<std::byte>(b));
    return r;
}

void test_raw_bytes_and_ragged_records() {
    graph_t g;
    const std::vector<std::vector<std::byte>> present = {
        raw({1, 0x00}),
        raw({1, 0xFF}),
        raw({2, 0x00, 0x01}),
        raw({2, 0xFF, 0xFF}),
        raw({2, 'a'}),  // ragged: the length byte overruns the record
        raw({2, 'a', 0x00}),
        raw({12, 's', 't', 'e', 'm', '_', 's', 't', 'e', 'm', '_', 's', 0x00}),
        raw({12, 's', 't', 'e', 'm', '_', 's', 't', 'e', 'm', '_', 's', 0x7F}),
        raw({12, 's', 't', 'e', 'm', '_', 's', 't', 'e', 'm', '_', 's', 0x80}),
        raw({12, 's', 't', 'e', 'm', '_', 's', 't', 'e', 'm', '_', 's', 0xFF}),
    };
    const std::vector<std::vector<std::byte>> absent = {
        raw({1, 0x01}),
        raw({2, 'a', 0x01}),
        raw({2, 0xFF, 0xFE}),
        raw({12, 's', 't', 'e', 'm', '_', 's', 't', 'e', 'm', '_', 's', 0x01}),
        raw({12, 's', 't', 'e', 'm', '_', 's', 't', 'e', 'm', '_', 's', 0xFE}),
    };
    bool registered = true;
    for (auto it = present.rbegin(); it != present.rend(); ++it)
        registered = g.register_vertex_key(*it, role_t::STORED_VALUE).has_value() && registered;
    check(registered, "(d) every raw record registers, 0x00, 0xFF and ragged included");
    bool all = true;
    for (const auto& k : present) all = g.find(k).has_value() && all;
    check(all, "(d) every raw record resolves");
    bool none = true;
    for (const auto& k : absent) none = !g.find(k).has_value() && none;
    check(none, "(d) a neighbouring raw record resolves to nothing");
    check(g.find(present[4]) != g.find(present[5]),
          "(d) a ragged record and the well-framed one it ties with are two vertices");
}

}  // namespace

int main() {
    test_prefix_ties_and_order();
    test_raw_bytes_and_ragged_records();
    return tr::testing::summary("child_prefix");
}
