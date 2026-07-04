/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Unit tests for tr::wire::key_view_t (key_view.hpp) — the canonical-key NAME
 * navigation the L4 graph dispatch and ACL-inheritance walks funnel through.
 * Those call sites are covered end-to-end by graph/acl/subtree/children tests;
 * this pins the navigation contract directly, including the segment-boundary
 * property (a byte-prefix of a valid key aligns only on a NAME boundary) and the
 * malformed-framing rejection that gates write-create.
 */

#include "libtracer/key_view.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace {

using tr::wire::key_view_t;

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// One NAME TLV record: [type=0x02, opt=0x00, u16 len (LE), payload...].
std::vector<std::byte> name_rec(std::string_view s) {
    std::vector<std::byte> r;
    r.push_back(std::byte{0x02});
    r.push_back(std::byte{0x00});
    r.push_back(static_cast<std::byte>(s.size() & 0xFF));
    r.push_back(static_cast<std::byte>((s.size() >> 8) & 0xFF));
    for (const char c : s) r.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    return r;
}

// A canonical key: the concatenated NAME records of `segs`.
std::vector<std::byte> make_key(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> k;
    for (const std::string_view s : segs) {
        const std::vector<std::byte> r = name_rec(s);
        k.insert(k.end(), r.begin(), r.end());
    }
    return k;
}

std::string_view as_str(std::span<const std::byte> b) {
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

bool bytes_eq(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

int main() {
    std::printf("key_view_t navigation:\n");

    // Root (empty key).
    {
        const key_view_t root{};
        check(root.empty(), "default key_view is the root (empty)");
        check(root.last_segment().empty(), "root last_segment is empty");
        check(root.parent().empty(), "root parent is the root");
        check(!root.is_ancestor_of(root), "root is not a strict ancestor of itself");
    }

    // Single segment "/a".
    {
        const std::vector<std::byte> a = make_key({"a"});
        const key_view_t k{a};
        check(!k.empty(), "single-segment key is not empty");
        check(as_str(k.last_segment()) == "a", "last_segment of /a is 'a'");
        check(k.parent().empty(), "parent of /a is the root");
    }

    // Three segments "/a/b/c".
    {
        const std::vector<std::byte> abc = make_key({"a", "b", "c"});
        const std::vector<std::byte> ab = make_key({"a", "b"});
        const std::vector<std::byte> a = make_key({"a"});
        const key_view_t k{abc};
        check(as_str(k.last_segment()) == "c", "last_segment of /a/b/c is 'c'");
        check(bytes_eq(k.parent().bytes(), ab), "parent of /a/b/c is /a/b");
        check(bytes_eq(k.parent().parent().bytes(), a), "grandparent of /a/b/c is /a");
        check(k.parent().parent().parent().empty(), "great-grandparent is the root");
    }

    // Ancestor / descendant, incl. the segment-boundary property.
    {
        const std::vector<std::byte> a = make_key({"a"});
        const std::vector<std::byte> ab = make_key({"a", "b"});
        const std::vector<std::byte> abc = make_key({"a", "b", "c"});
        const std::vector<std::byte> ab_word = make_key({"ab"});  // NOT a descendant of /a
        check(key_view_t{a}.is_ancestor_of(key_view_t{ab}), "/a is an ancestor of /a/b");
        check(key_view_t{a}.is_ancestor_of(key_view_t{abc}), "/a is an ancestor of /a/b/c");
        check(!key_view_t{ab}.is_ancestor_of(key_view_t{a}), "/a/b is not an ancestor of /a");
        check(!key_view_t{a}.is_ancestor_of(key_view_t{a}), "not a strict ancestor of itself");
        // The load-bearing property: "/ab" shares no NAME boundary with "/a", so
        // the byte-prefix test must reject it (differing length header).
        check(!key_view_t{a}.is_ancestor_of(key_view_t{ab_word}),
              "/a is NOT an ancestor of /ab (segment-boundary property)");
    }

    // Direct child record extraction.
    {
        const std::vector<std::byte> a = make_key({"a"});
        const std::vector<std::byte> ab = make_key({"a", "b"});
        const std::vector<std::byte> abc = make_key({"a", "b", "c"});
        const std::vector<std::byte> b_rec = name_rec("b");
        const auto direct = key_view_t{ab}.child_record_under(key_view_t{a});
        check(direct.has_value(), "/a/b is a direct child of /a");
        check(direct && bytes_eq(*direct, b_rec), "the child record IS /b's NAME encoding");
        check(!key_view_t{abc}.child_record_under(key_view_t{a}).has_value(),
              "/a/b/c is NOT a direct child of /a (deeper descendant)");
        check(!key_view_t{a}.child_record_under(key_view_t{a}).has_value(),
              "a key is not its own direct child");
        check(!key_view_t{a}.child_record_under(key_view_t{ab}).has_value(),
              "/a is not a direct child of its own descendant /a/b");
    }

    // Level split (mkdir -p order) + malformed-framing rejection.
    {
        const std::vector<std::byte> abc = make_key({"a", "b", "c"});
        std::vector<key_view_t> levels;
        check(key_view_t{abc}.split_levels(levels), "split_levels of a well-framed key succeeds");
        check(levels.size() == 3, "three levels for /a/b/c");
        check(levels.size() == 3 && bytes_eq(levels[0].bytes(), make_key({"a"})), "level 0 = /a");
        check(levels.size() == 3 && bytes_eq(levels[1].bytes(), make_key({"a", "b"})),
              "level 1 = /a/b");
        check(levels.size() == 3 && bytes_eq(levels[2].bytes(), abc), "level 2 = /a/b/c");

        std::vector<key_view_t> empty_levels;
        check(!key_view_t{}.split_levels(empty_levels), "split_levels of the root fails");
        check(empty_levels.empty(), "root split appends nothing");

        // A ragged key: a valid /a followed by a truncated record (header claims a
        // payload that runs past the end). Must reject and append nothing.
        std::vector<std::byte> ragged = make_key({"a"});
        ragged.push_back(std::byte{0x02});
        ragged.push_back(std::byte{0x00});
        ragged.push_back(std::byte{0x09});  // len=9 but no payload follows
        ragged.push_back(std::byte{0x00});
        std::vector<key_view_t> ragged_levels;
        check(!key_view_t{ragged}.split_levels(ragged_levels),
              "split_levels rejects ragged framing");
        check(ragged_levels.empty(), "ragged split appends nothing");
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures;
}
