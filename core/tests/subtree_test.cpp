/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0005 — subtree subscriptions (vertical bubbling), branch-write
 * decomposition, and write-creates. Every subscription observes writes to its
 * vertex AND to any descendant: a write at V fans out to subscribers at V and
 * at each ancestor of V, delivering the written TLV as-is. The idle write path
 * never walks ancestors (asserted via graph_t::ancestor_walks()). A POINT
 * payload written to V decomposes: each value-carrying node lands at the
 * corresponding descendant vertex as a refcount SUBVIEW of the written frame
 * (zero copy), creating missing vertices mkdir-p style (CREATE-gated), and each
 * covered subscription point is notified once with its slice. A data write to a
 * nonexistent path creates it. The branch is NOT a transaction: admission
 * (ACL/creation) is all-or-nothing, application is per-leaf (each leaf a
 * consistent refcounted snapshot) — the atomicity non-promise of RFC-0005.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// A view_t over a fresh, owned heap segment holding `bytes`.
view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

view_t make_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> v;
    v.reserve(bytes.size());
    for (const std::uint8_t b : bytes) v.push_back(std::byte{b});
    return make_value(v);
}

// --- branch-write (POINT tree) byte builders --------------------------------

// A VALUE TLV over raw payload bytes.
std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> payload) {
    std::vector<std::byte> body;
    for (const std::uint8_t b : payload) body.push_back(std::byte{b});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, body);
    return out;
}

// A POINT TLV: NAME `name` first, then the pre-encoded `children` bytes
// (concatenated VALUE / nested POINT TLVs).
std::vector<std::byte> point_tlv(std::string_view name, std::span<const std::byte> children) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, name);
    body.insert(body.end(), children.begin(), children.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::POINT, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> cat(std::initializer_list<std::vector<std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

bool same_bytes(const view_t& v, std::span<const std::byte> expect) {
    const std::span<const std::byte> got = v.bytes();
    return got.size() == expect.size() && std::memcmp(got.data(), expect.data(), got.size()) == 0;
}

// True iff `v`'s bytes live INSIDE `frame`'s segment window — the zero-copy
// (refcount subview, no byte copy) assertion.
bool is_subview_of(const view_t& v, const view_t& frame) {
    const std::span<const std::byte> f = frame.bytes();
    const std::span<const std::byte> s = v.bytes();
    return s.data() >= f.data() && s.data() + s.size() <= f.data() + f.size();
}

// --- tests -------------------------------------------------------------------

void test_bubbling_and_idle_walk() {
    std::printf("vertical bubbling + near-free-when-idle (RFC-0005):\n");
    graph_t g;
    vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/a/b"), role_t::STORED_VALUE);
    vertex_handle_t abc = g.register_vertex(path_t("/a/b/c"), role_t::STORED_VALUE);
    vertex_handle_t x = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);

    // Idle: nobody listens above — a write never walks ancestors.
    check(g.write(abc, make_value({0x01})).has_value(), "leaf write with no subscribers");
    check(g.ancestor_walks() == 0, "no subscriber anywhere => no ancestor walk");

    std::vector<std::vector<std::byte>> at_a;
    std::vector<std::vector<std::byte>> at_x;
    check(g.subscribe(path_t("/a"),
                      [&](const rope_t& v) {
                          at_a.emplace_back(v.only().bytes().begin(), v.only().bytes().end());
                      })
              .has_value(),
          "subscribe callback at ancestor /a");
    check(g.subscribe(path_t("/x"),
                      [&](const rope_t& v) {
                          at_x.emplace_back(v.only().bytes().begin(), v.only().bytes().end());
                      })
              .has_value(),
          "subscribe callback at unrelated /x");

    const std::vector<std::byte> written{std::byte{0x01}, std::byte{0x00}, std::byte{0x02},
                                         std::byte{0x00}, std::byte{0xAB}, std::byte{0xCD}};
    check(g.write(abc, make_value(written)).has_value(), "leaf write under a subscribed ancestor");
    check(g.ancestor_walks() == 1, "listening ancestor => exactly one walk");
    check(at_a.size() == 1, "ancestor subscriber notified once");
    check(at_a.size() == 1 && std::ranges::equal(at_a[0], written),
          "ancestor receives the written TLV as-is");
    check(at_x.empty(), "unrelated subtree's subscriber sees nothing");

    // A write at the subscription point itself is the trivial (leaf) case.
    check(g.write(a, make_value({0x09, 0x00, 0x00, 0x00})).has_value(), "write at /a itself");
    check(at_a.size() == 2, "subscriber at /a also observes writes to /a");
    check(g.ancestor_walks() == 1, "write at /a (nothing above) does not walk");

    // Unsubscribe (clear the slot) => the walk stops.
    check(g.write(path_t("/a:subscribers[0]"), make_value({0x09, 0x00, 0x00, 0x00})).has_value(),
          "clear /a:subscribers[0] (unsubscribe)");
    check(g.write(abc, make_value({0x05})).has_value(), "leaf write after unsubscribe");
    check(g.ancestor_walks() == 1, "unsubscribed => the walk stops again");
    check(at_a.size() == 2, "cleared slot no longer delivers");
    (void)x;
}

void test_bubbling_to_late_created_descendant() {
    std::printf("bubbling from a vertex created AFTER the subscription:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    std::size_t hits = 0;
    check(g.subscribe(path_t("/a"), [&](const rope_t&) { ++hits; }).has_value(),
          "subscribe at /a first");
    // The descendant is created afterwards (write-creates) — its creation-time
    // ancestor-listener sum must still route its writes up.
    check(g.write(path_t("/a/new/leaf"), make_value({0x42})).has_value(),
          "write-creates /a/new/leaf under the live subscription");
    check(hits == 1, "the late-created descendant's write bubbles to /a");
}

void test_remote_ancestor_subscriber() {
    std::printf("remote ancestor subscriber (fan-out via return_route):\n");
    graph_t g;
    vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    vertex_handle_t ab = g.register_vertex(path_t("/a/b"), role_t::STORED_VALUE);

    std::vector<std::byte> route{std::byte{0x06}, std::byte{0x40}, std::byte{0x00},
                                 std::byte{0x00}};
    std::size_t deliveries = 0;
    std::string seen_link;
    std::vector<std::byte> seen_value;
    std::vector<std::byte> seen_route;
    g.set_remote_delivery_sink([&](const tr::graph::remote_delivery_t& d, const rope_t& v) {
        ++deliveries;
        seen_link.assign(d.link);
        seen_value.assign(v.only().bytes().begin(), v.only().bytes().end());
        seen_route.assign(d.return_route.bytes().begin(), d.return_route.bytes().end());
    });
    check(g.subscribe_wire(a, make_value({0x04, 0x40, 0x00, 0x00}), make_value(route), "lnk0")
              .has_value(),
          "bind a REMOTE subscriber at the ancestor /a");

    const std::vector<std::byte> written{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
                                         std::byte{0x00}, std::byte{0x7F}};
    check(g.write(ab, make_value(written)).has_value(), "write at the descendant /a/b");
    check(deliveries == 1, "remote sink fired once (bubbled to /a's remote edge)");
    check(seen_link == "lnk0", "delivery carries the subscriber's link");
    check(std::ranges::equal(seen_value, written), "delivery payload is the written TLV as-is");
    check(std::ranges::equal(seen_route, route), "delivery retraces the stored return_route");
}

void test_branch_write_decomposition() {
    std::printf("branch-write (POINT) decomposition:\n");
    graph_t g;
    vertex_handle_t s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    vertex_handle_t st = g.register_vertex(path_t("/s/t"), role_t::STORED_VALUE);

    std::vector<std::vector<std::byte>> at_s;
    std::vector<view_t> at_st;
    check(g.subscribe(path_t("/s"),
                      [&](const rope_t& v) {
                          at_s.emplace_back(v.only().bytes().begin(), v.only().bytes().end());
                      })
              .has_value(),
          "subscribe at the branch root /s");
    check(g.subscribe(path_t("/s/t"), [&](const rope_t& v) { at_st.push_back(v.only()); })
              .has_value(),
          "subscribe at the leaf /s/t");

    // POINT{ NAME "s", VALUE 07, POINT{ NAME "t", VALUE AA BB },
    //                            POINT{ NAME "u", VALUE CC } }  — /s/u not yet registered.
    const std::vector<std::byte> t_val = value_tlv({0xAA, 0xBB});
    const std::vector<std::byte> branch = point_tlv(
        "s", cat({value_tlv({0x07}), point_tlv("t", t_val), point_tlv("u", value_tlv({0xCC}))}));
    const view_t frame = make_value(branch);
    check(g.write(s, frame).has_value(), "branch write at /s succeeds");

    // Values are the truth at the vertices where they land (one store per vertex).
    const auto r_s = g.read(s);
    check(r_s.has_value() && same_bytes(r_s->only(), value_tlv({0x07})),
          "read /s returns the root's own VALUE TLV");
    const auto r_t = g.read(st);
    check(r_t.has_value() && same_bytes(r_t->only(), t_val),
          "read /s/t returns the leaf's VALUE TLV");
    check(r_t.has_value() && is_subview_of(r_t->only(), frame),
          "leaf store is a refcount SUBVIEW of the written frame (zero copy)");
    const auto r_u = g.read(path_t("/s/u"));
    check(r_u.has_value() && same_bytes(r_u->only(), value_tlv({0xCC})),
          "write-created /s/u holds its decomposed VALUE");

    // Notifications: one per covered subscription point, with its slice.
    check(at_s.size() == 1, "root subscriber notified once for the whole branch");
    check(at_s.size() == 1 && std::ranges::equal(at_s[0], branch),
          "root subscriber receives the written branch TLV as-is");
    check(at_st.size() == 1, "leaf subscriber notified once");
    check(at_st.size() == 1 && same_bytes(at_st[0], t_val),
          "leaf subscriber receives its VALUE slice");
    check(at_st.size() == 1 && is_subview_of(at_st[0], frame),
          "leaf notification is a subview of the written frame (zero copy)");

    // Read-after-notify invariant: the LKV a read serves is never behind what the
    // subscriber just saw (it IS the same refcounted slice here).
    check(r_t.has_value() && at_st.size() == 1 &&
              r_t->only().bytes().data() == at_st[0].bytes().data(),
          "read and notification serve the same stored slice");
}

void test_branch_write_strictness() {
    std::printf("branch-write strictness (shape / addressing errors):\n");
    graph_t g;
    vertex_handle_t s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);

    // Root NAME must equal the target vertex's leaf segment.
    const auto wrong = g.write(s, make_value(point_tlv("zz", value_tlv({0x01}))));
    check(!wrong.has_value() && wrong.error() == status_t::INVALID_PATH,
          "root NAME mismatch => INVALID_PATH");

    // At most one VALUE per node.
    const auto two = g.write(s, make_value(point_tlv("s", cat({value_tlv({1}), value_tlv({2})}))));
    check(!two.has_value() && two.error() == status_t::TYPE_MISMATCH,
          "two VALUE children on one node => TYPE_MISMATCH");

    // Only NAME / VALUE / POINT children are admitted in a branch write.
    std::vector<std::byte> alien;
    tr::wire::emit_tlv(alien, type_t::TIME, opt_t{}, std::vector<std::byte>(8));
    const auto bad = g.write(s, make_value(point_tlv("s", alien)));
    check(!bad.has_value() && bad.error() == status_t::TYPE_MISMATCH,
          "a non-{NAME,VALUE,POINT} child => TYPE_MISMATCH");

    // A value-free branch is a no-op write (nothing stored, nothing delivered).
    std::size_t hits = 0;
    check(g.subscribe(path_t("/s"), [&](const rope_t&) { ++hits; }).has_value(), "subscribe at /s");
    check(g.write(s, make_value(point_tlv("s", point_tlv("t", {})))).has_value(),
          "value-free branch write is accepted");
    check(hits == 0, "value-free branch delivers nothing");
    check(!g.read(path_t("/s/t")).has_value(), "value-free branch stores nothing");
}

void test_write_creates() {
    std::printf("write-creates (mkdir-p, CREATE-gated):\n");
    graph_t g;
    // A data write to a nonexistent path creates it (and intermediates).
    const std::vector<std::byte> val{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
                                     std::byte{0x00}, std::byte{0x2A}};
    check(g.write(path_t("/new/deep/leaf"), make_value(val)).has_value(),
          "write to a nonexistent path creates it");
    const auto r = g.read(path_t("/new/deep/leaf"));
    check(r.has_value() && same_bytes(r->only(), val), "created leaf serves the written value");
    check(g.find(path_t::parse("/new/deep")->key()).has_value(),
          "intermediate levels are created too (mkdir-p)");

    // A :field write to a nonexistent vertex does NOT create.
    const auto f = g.write(path_t("/nope:settings.priority"), make_value({1, 0, 1, 0, 1}));
    check(!f.has_value() && f.error() == status_t::NOT_FOUND,
          "field write to a nonexistent vertex stays NOT_FOUND");
    check(!g.find(path_t::parse("/nope")->key()).has_value(), "field write created nothing");
}

void test_write_creates_acl_gate() {
    std::printf("write-creates CREATE-ACL gate (denied => PERMISSION_DENIED):\n");
    graph_t g;
    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    // Every caller (including local) resolves to a subject — enforcement is on.
    g.set_subject_resolver([](std::string_view) -> std::optional<subject_token_t> {
        return subject_token_t{std::byte{'u'}};
    });

    // /p grants WRITE (with INHERIT) but NOT CREATE — creating below /p is denied.
    // Built via the typed ADR-0050 surface (encode_acl) — no hand-rolled ACE bytes.
    std::vector<std::byte> everyone;
    for (const char c : std::string_view("EVERYONE@")) everyone.push_back(std::byte(c));
    const std::vector<tr::graph::ace_t> aces{
        {.flags = tr::graph::kAceInherit,
         .subject = everyone,
         .access_mask = static_cast<std::uint32_t>(acl_right_t::WRITE)}};
    const std::vector<std::byte> acl = tr::graph::encode_acl(aces);
    check(g.write(path_t("/p:acl"), make_value(acl)).has_value(),
          "install a WRITE-only (no CREATE) ACL on /p");

    const auto denied = g.write(path_t("/p/child"), make_value({0x01, 0, 1, 0, 9}));
    check(!denied.has_value() && denied.error() == status_t::PERMISSION_DENIED,
          "write-create under /p without the CREATE right => PERMISSION_DENIED");
    check(!g.find(path_t::parse("/p/child")->key()).has_value(), "denied create made no vertex");

    // Writing to the existing /p itself is still allowed (WRITE granted).
    check(g.write(path_t("/p"), make_value({0x01, 0, 1, 0, 9})).has_value(),
          "plain write to /p still allowed by the WRITE grant");
}

void test_branch_write_acl_admission() {
    std::printf("branch-write admission is all-or-nothing under ACL:\n");
    graph_t g;
    vertex_handle_t s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    vertex_handle_t st = g.register_vertex(path_t("/s/t"), role_t::STORED_VALUE);
    vertex_handle_t su = g.register_vertex(path_t("/s/u"), role_t::STORED_VALUE);
    g.set_subject_resolver([](std::string_view) -> std::optional<subject_token_t> {
        return subject_token_t{std::byte{'u'}};
    });
    // Close /s/u to writes (an ACL granting only READ — any present ACE closes it),
    // built via the typed ADR-0050 surface (encode_acl).
    std::vector<std::byte> everyone;
    for (const char c : std::string_view("EVERYONE@")) everyone.push_back(std::byte(c));
    const std::vector<tr::graph::ace_t> aces{
        {.subject = everyone, .access_mask = static_cast<std::uint32_t>(acl_right_t::READ)}};
    const std::vector<std::byte> acl = tr::graph::encode_acl(aces);
    check(g.write(path_t("/s/u:acl"), make_value(acl)).has_value(), "close /s/u to writes");

    const std::vector<std::byte> branch =
        point_tlv("s", cat({point_tlv("t", value_tlv({0x11})), point_tlv("u", value_tlv({0x22}))}));
    const auto denied = g.write(s, make_value(branch));
    check(!denied.has_value() && denied.error() == status_t::PERMISSION_DENIED,
          "one denied landing site rejects the whole branch");
    check(!g.read(st).has_value() && !g.read(su).has_value(),
          "nothing landed anywhere (admission is atomic)");
}

}  // namespace

int main() {
    std::printf("libtracer subtree-subscription tests (RFC-0005):\n");
    test_bubbling_and_idle_walk();
    test_bubbling_to_late_created_descendant();
    test_remote_ancestor_subscriber();
    test_branch_write_decomposition();
    test_branch_write_strictness();
    test_write_creates();
    test_write_creates_acl_gate();
    test_branch_write_acl_admission();
    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
