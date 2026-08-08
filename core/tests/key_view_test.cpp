/**
 * @file
 * @brief Unit tests for tr::wire::key_view_t (key_view.hpp) — the canonical-key NAME navigation the
 *        L4 graph dispatch and ACL-inheritance walks funnel through.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
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

/** @brief One NAME TLV record: [type=0x02, opt=0x00, u16 len (LE), payload...]. */
std::vector<std::byte> name_rec(std::string_view s) {
    std::vector<std::byte> r;
    r.push_back(std::byte{0x02});
    r.push_back(std::byte{0x00});
    r.push_back(static_cast<std::byte>(s.size() & 0xFF));
    r.push_back(static_cast<std::byte>((s.size() >> 8) & 0xFF));
    for (const char c : s) r.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    return r;
}

/** @brief A canonical key: the concatenated NAME records of `segs`. */
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

    // ---- The shared record accessors (#888) -------------------------------------------
    // These are the framing decode the graph's Composite descent, the router's mount
    // descent and transport_vertex used to hand-roll one copy each. The contract pinned
    // here is exactly what those copies did, including their ragged edges.

    // record_end / record_from over a well-framed key.
    {
        const std::vector<std::byte> k3 = make_key({"a", "bb", "ccc"});  // 5 + 6 + 7 = 18 bytes
        const key_view_t k{k3};
        check(k3.size() == 18, "fixture /a/bb/ccc is 18 bytes");
        check(k.record_end(0) == 5, "record_end(0) = 5 (4-byte header + 'a')");
        check(k.record_end(5) == 11, "record_end(5) = 11");
        check(k.record_end(11) == 18, "record_end(11) = 18 (records tile the key)");
        check(k.record_end(18) == 0, "record_end at the key's end is ragged (0)");

        const auto r1 = k.record_from(5);
        check(r1.has_value(), "record_from(5) is engaged");
        check(r1 && r1->begin == 5 && r1->end == 11, "record_from(5) spans [5, 11)");
        check(r1 && as_str(r1->payload) == "bb", "record_from(5) carries 'bb'");
        check(!k.record_from(18).has_value(), "record_from past the last record is nullopt");
        check(!key_view_t{}.record_from(0).has_value(), "the root has no record 0");

        // A zero-length payload is a WELL-FRAMED record, not a ragged one.
        const std::vector<std::byte> empty_seg = make_key({""});
        check(key_view_t{empty_seg}.record_end(0) == 4, "a len-0 record ends at 4");
    }

    // record_end's ragged edges, one per shape the hand-walks guarded against.
    {
        std::vector<std::byte> short_hdr = make_key({"a"});
        short_hdr.push_back(std::byte{0x02});  // 2 of the 4 header bytes, then nothing
        short_hdr.push_back(std::byte{0x00});
        check(key_view_t{short_hdr}.record_end(5) == 0, "a truncated HEADER is ragged");

        std::vector<std::byte> over_len = make_key({"a"});
        over_len.push_back(std::byte{0x02});
        over_len.push_back(std::byte{0x00});
        over_len.push_back(std::byte{0x09});  // len = 9, but no payload follows
        over_len.push_back(std::byte{0x00});
        check(key_view_t{over_len}.record_end(5) == 0, "an OVERSIZED length is ragged");
        check(key_view_t{over_len}.record_end(0) == 5, "the well-framed record before it is not");

        // graph.cpp's `segment_end` ragged-tail RULE, expressed in the accessor it now
        // reads the framing through: a record whose REMAINDER is ragged swallows the
        // remainder, so the Composite decomposition and key_view_t::parent() agree.
        const key_view_t rag{over_len};
        check(rag.record_end(0) != 0 && rag.record_end(rag.record_end(0)) == 0,
              "a ragged remainder is what makes the tail glue onto the last good record");
    }

    // record_cursor_t: the indexed walk the mount descent asks by segment number.
    {
        const std::vector<std::byte> k3 = make_key({"a", "bb", "ccc"});
        key_view_t::record_cursor_t cur{key_view_t{k3}};
        check(cur.at(0) && as_str(cur.at(0)->payload) == "a", "cursor at(0) = 'a'");
        check(cur.at(1) && as_str(cur.at(1)->payload) == "bb", "cursor at(1) = 'bb'");
        check(cur.at(2) && as_str(cur.at(2)->payload) == "ccc", "cursor at(2) = 'ccc'");
        check(!cur.at(3).has_value(), "cursor past the last record is nullopt");
        // Asking BEHIND the walk restarts it and must give the same answer.
        check(cur.at(0) && as_str(cur.at(0)->payload) == "a", "a descending ask still answers 'a'");
        check(cur.at(2) && cur.at(2)->begin == 11, "re-ascending re-finds record 2 at offset 11");
        check(cur.at(2) && cur.at(2)->end == 18, "the memoized repeat is the same record");

        // Every index agrees with a fresh cursor — the incremental walk and the
        // rescan-from-zero it replaces cannot disagree.
        for (std::size_t i = 0; i < 4; ++i) {
            key_view_t::record_cursor_t fresh{key_view_t{k3}};
            const auto a = fresh.at(i);
            const auto b = cur.at(i);
            check(a.has_value() == b.has_value() &&
                      (!a || (a->begin == b->begin && a->end == b->end)),
                  "incremental and from-scratch cursors agree at every index");
        }

        // end_of: the strip-K residual offset. 0 records = 0; all records = the key's size.
        key_view_t::record_cursor_t e{key_view_t{k3}};
        check(e.end_of(0) == 0, "end_of(0) is 0");
        check(e.end_of(1) == 5, "end_of(1) is 5");
        check(e.end_of(3) == 18, "end_of(3) is the whole key");
        check(e.end_of(4) == 18, "end_of past the last record clamps to the key's size");
    }

    // The cursor's ragged edges, which are `subscribe_toward`'s old `rec_off`/`key_at`
    // rules: a walk that meets a ragged record answers nullopt from there on, and
    // `end_of` past it reports the KEY SIZE — while `end_of` AT it reports the ragged
    // tail's offset, which is what the old rescan returned.
    {
        std::vector<std::byte> ragged = make_key({"a", "bb"});  // 5 + 6 = 11 well-framed
        ragged.push_back(std::byte{0x02});
        ragged.push_back(std::byte{0x00});
        ragged.push_back(std::byte{0x09});  // len = 9, nothing follows
        ragged.push_back(std::byte{0x00});
        key_view_t::record_cursor_t cur{key_view_t{ragged}};
        check(ragged.size() == 15, "ragged fixture is 15 bytes");
        check(cur.at(1) && as_str(cur.at(1)->payload) == "bb", "records before the tail decode");
        check(!cur.at(2).has_value(), "the ragged record itself is nullopt");
        check(!cur.at(9).has_value(), "so is everything past it");
        check(cur.end_of(2) == 11, "end_of(2) is where the ragged tail starts");
        check(cur.end_of(3) == 15, "end_of past the ragged record is the key's size");
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures;
}
