/**
 * @file
 * @brief The field surface's READ/WRITE dispatch parity guards (#869).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The per-field selector-shape rules used to exist as two independently hand-written
 * if-chains — `graph_t::field_write` and the IIFE inside `graph_t::read`'s field overload —
 * spelling the same rule three different ways and, in places, already answering differently.
 * The shapes are now ONE classification (`field_selector` / `whole_field` / `app_field_sel`
 * in graph.cpp) that both doors switch on; the per-side ANSWERS stay separate because they
 * are genuinely asymmetric.
 *
 * This file is the characterization net that makes that dedup safe, and the record of the
 * divergences it deliberately did NOT close. Every answer below is the answer main gave at
 * `fea04a58`, whether or not it is the answer it SHOULD give: three of them are marked
 * DIVERGENCE and are pinned precisely so that changing one is a deliberate, RFC-shaped act
 * (each changes an error code that leaves the device — docs/spec/v1.md §3 incorporates
 * docs/reference/05-protocol-tlvs.md in full, including the `:`-field control surface).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::field_path_t;
using tr::graph::field_step_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;

/** @brief `s`'s bytes — the subject-token spelling the test resolver hands back. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

/** @brief The test resolver (ADR-0018): the caller context IS the subject token. */
std::expected<tr::graph::subject_token_t, tr::wire::err_t> caller_is_subject(
    std::string_view caller) {
    return as_bytes(caller);
}

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A view_t over a fresh, owned heap segment holding @p bytes. */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

tr::view::view_t make_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> v;
    v.reserve(bytes.size());
    for (std::uint8_t b : bytes) v.push_back(std::byte{b});
    return make_value(v);
}

/** @brief A SUBSCRIBER TLV naming a single-segment target path. */
tr::view::view_t subscriber_tlv(std::string_view target_segment) {
    std::vector<std::byte> name_bytes;
    for (char c : target_segment)
        name_bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    tr::wire::tlv_t name{.type = tr::wire::type_t::NAME, .payload = name_bytes};
    tr::wire::tlv_t path{.type = tr::wire::type_t::PATH};
    path.opt.pl = true;
    path.children.push_back(name);
    tr::wire::tlv_t sub{.type = tr::wire::type_t::SUBSCRIBER};
    sub.opt.pl = true;
    sub.children.push_back(path);
    return make_value(tr::wire::encode(sub));
}

/** @brief A SUBSCRIBER TLV with NO PATH child — the shape the two `field_write` doors refuse
 *         for want of a `target_key` and `subscribe_wire` accepts (it clears the key). */
tr::view::view_t subscriber_tlv_no_target() {
    tr::wire::tlv_t sub{.type = tr::wire::type_t::SUBSCRIBER};
    sub.opt.pl = true;
    return make_value(tr::wire::encode(sub));
}

/**
 * @brief A POINT carrying the SAME `PATH{NAME}` child a SUBSCRIBER would — everything a door
 *        needs EXCEPT the type code.
 *
 * The plain VALUE payload is not enough to guard the shared type check: it has no PATH child,
 * so the two `field_write` doors would refuse it downstream on the missing `target_key` and
 * answer TYPE_MISMATCH either way. (Measured: deleting the type check from
 * `parse_wire_subscriber` leaves a VALUE-based row green at both of them.) This shape has a
 * target, so only the type code can refuse it.
 */
tr::view::view_t non_subscriber_with_path(std::string_view target_segment) {
    std::vector<std::byte> name_bytes;
    for (char c : target_segment)
        name_bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    tr::wire::tlv_t name{.type = tr::wire::type_t::NAME, .payload = name_bytes};
    tr::wire::tlv_t path{.type = tr::wire::type_t::PATH};
    path.opt.pl = true;
    path.children.push_back(name);
    tr::wire::tlv_t point{.type = tr::wire::type_t::POINT};
    point.opt.pl = true;
    point.children.push_back(path);
    return make_value(tr::wire::encode(point));
}

/** @brief The selector spellings a field step can carry. */
enum class sel_t : std::uint8_t { BARE, APPEND, SLOT, WILDCARD };

/**
 * @brief Build a field path `<head>[<sel>]` optionally followed by plain `tail` steps.
 *
 * `[*]` is NOT text-path syntax (RFC-0004 §C makes it a FIELD TLV with index_mode=WILDCARD),
 * so it is built the way the wire decoder builds it — `indexed` set, `wildcard` set, `index`
 * left at 0. That is exactly #579's shape.
 */
field_path_t fp(std::string_view head, sel_t sel, std::initializer_list<const char*> tail = {}) {
    field_path_t f;
    field_step_t s;
    s.name = std::string(head);
    switch (sel) {
        case sel_t::BARE:
            break;
        case sel_t::APPEND:
            s.indexed = true;
            s.append = true;
            break;
        case sel_t::SLOT:
            s.indexed = true;
            s.index = 0;
            break;
        case sel_t::WILDCARD:
            s.indexed = true;
            s.wildcard = true;
            break;
    }
    f.steps.push_back(std::move(s));
    for (const char* t : tail) {
        field_step_t ts;
        ts.name = t;
        f.steps.push_back(std::move(ts));
    }
    return f;
}

/**
 * @brief The outcome of one door call: the failing `status_t`, or `nullopt` for success.
 *
 * `status_t` has no OK member — success is the value side of `result_t` — so the guards
 * below compare optionals, and `kOk` is the empty one.
 */
using outcome_t = std::optional<status_t>;
constexpr outcome_t kOk{};

/** @brief The outcome of a field read. */
outcome_t read_status(const graph_t& g, vertex_handle_t v, const field_path_t& f,
                      std::string_view caller) {
    const auto r = g.read(v, f, caller);
    if (r) return kOk;
    return r.error();
}

/** @brief The outcome of a field write. */
outcome_t write_status(graph_t& g, vertex_handle_t v, const field_path_t& f,
                       const tr::view::view_t& value, std::string_view caller) {
    const auto r = g.write(v, f, tr::view::rope_t{value}, caller);
    if (r) return kOk;
    return r.error();
}

const char* name_of(outcome_t o) {
    if (!o) return "OK";
    switch (*o) {
        case status_t::NOT_FOUND:
            return "NOT_FOUND";
        case status_t::INVALID_PATH:
            return "INVALID_PATH";
        case status_t::PERMISSION_DENIED:
            return "PERMISSION_DENIED";
        case status_t::SCHEMA_NOT_FOUND:
            return "SCHEMA_NOT_FOUND";
        case status_t::TYPE_MISMATCH:
            return "TYPE_MISMATCH";
        default:
            return "OTHER";
    }
}

void expect(outcome_t got, outcome_t want, std::string_view what) {
    if (got == want) {
        check(true, what);
        return;
    }
    std::printf("  [FAIL] %.*s — got %s, want %s\n", static_cast<int>(what.size()), what.data(),
                name_of(got), name_of(want));
    ++g_failures;
}

/**
 * @brief The full field × selector matrix, both doors, for a caller with no ACL restriction.
 *
 * This is the net: any shape rule the #869 dedup moved to a shared classifier must keep
 * answering exactly what it answered before. Rows marked DIVERGENCE are the read/write
 * disagreements found while doing it — pinned, not fixed.
 */
void test_field_shape_matrix() {
    std::printf("field shape matrix — read and write, one row per (field, selector):\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "kp", .access = app_access_t::RW});
    g.set_app_fields(v, std::move(table));
    const tr::view::view_t val = make_value({0x01, 0x00, 0x01, 0x00, 0x2a});
    constexpr std::string_view kOwner{};  // the empty (owner) caller — no ACL is installed

    // `:acl` — served whole. The write door resolves an unaccepted shape ABOVE its gate.
    // A recognised name with an EMPTY facet is NOT_FOUND, not SCHEMA_NOT_FOUND — which is
    // also what proves the bare shape RESOLVED (it reached `read_acl`) rather than falling
    // through to the terminal answer, as every unaccepted shape below it does.
    expect(read_status(g, v, fp("acl", sel_t::BARE), kOwner), status_t::NOT_FOUND,
           "read :acl on a vertex with no ACL is NOT_FOUND (the shape resolved)");
    expect(read_status(g, v, fp("acl", sel_t::SLOT), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :acl[0]");
    expect(read_status(g, v, fp("acl", sel_t::APPEND), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :acl[]");
    expect(read_status(g, v, fp("acl", sel_t::BARE, {"bogus"}), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :acl.bogus");
    expect(write_status(g, v, fp("acl", sel_t::SLOT), val, kOwner), status_t::SCHEMA_NOT_FOUND,
           "write :acl[0]");
    expect(write_status(g, v, fp("acl", sel_t::BARE, {"bogus"}), val, kOwner),
           status_t::SCHEMA_NOT_FOUND, "write :acl.bogus");

    // `:identity` — whole namespace resolves above the READ gate; no write surface at all.
    // A keypair has to be installed first: a keyless node answers SCHEMA_NOT_FOUND for the
    // BARE spelling too, which would make the indexed row pass without the shape rule.
    const std::vector<std::byte> key(32, std::byte{0x11});
    check(g.set_identity(1, key).has_value(), "a keypair is installed");
    expect(read_status(g, v, fp("identity", sel_t::BARE), kOwner), kOk, "read :identity");
    expect(read_status(g, v, fp("identity", sel_t::SLOT), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :identity[0]");
    expect(read_status(g, v, fp("identity", sel_t::BARE, {"pubkey"}), kOwner),
           status_t::SCHEMA_NOT_FOUND, "read :identity.pubkey");
    expect(write_status(g, v, fp("identity", sel_t::BARE), val, kOwner), status_t::SCHEMA_NOT_FOUND,
           "write :identity");

    // `:schema` — one synthesized POINT, served whole; read-only.
    expect(read_status(g, v, fp("schema", sel_t::BARE), kOwner), kOk, "read :schema");
    expect(read_status(g, v, fp("schema", sel_t::SLOT), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :schema[0]");
    expect(read_status(g, v, fp("schema", sel_t::WILDCARD), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :schema[*]");
    expect(write_status(g, v, fp("schema", sel_t::BARE), val, kOwner), status_t::SCHEMA_NOT_FOUND,
           "write :schema");

    // `:children` — read enumerates (bare and `[]`), write CREATES (`[]` only). The
    // read/write asymmetry that a single shape table must NOT flatten.
    expect(read_status(g, v, fp("children", sel_t::BARE), kOwner), kOk, "read :children");
    expect(read_status(g, v, fp("children", sel_t::APPEND), kOwner), kOk, "read :children[]");
    expect(read_status(g, v, fp("children", sel_t::SLOT), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :children[0]");
    expect(read_status(g, v, fp("children", sel_t::WILDCARD), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :children[*]");
    expect(read_status(g, v, fp("children", sel_t::APPEND, {"bogus"}), kOwner),
           status_t::SCHEMA_NOT_FOUND, "read :children[].bogus");
    expect(write_status(g, v, fp("children", sel_t::BARE), val, kOwner), status_t::SCHEMA_NOT_FOUND,
           "write :children");
    expect(write_status(g, v, fp("children", sel_t::SLOT), val, kOwner), status_t::SCHEMA_NOT_FOUND,
           "write :children[0]");
    expect(write_status(g, v, fp("children", sel_t::WILDCARD), val, kOwner),
           status_t::SCHEMA_NOT_FOUND, "write :children[*]");
    expect(write_status(g, v, fp("children", sel_t::APPEND, {"bogus"}), val, kOwner),
           status_t::SCHEMA_NOT_FOUND, "write :children[].bogus (#581)");
    // A `[]` write with a non-SPEC payload reaches create_child and is refused there — which
    // is what proves the shape gate above let it through rather than short-circuiting.
    expect(write_status(g, v, fp("children", sel_t::APPEND), val, kOwner), status_t::TYPE_MISMATCH,
           "write :children[] reaches create_child (TYPE_MISMATCH on a non-SPEC)");

    // `:settings` — the core namespace is EMPTY (RFC-0022 §3.B); `settings.app.` is the
    // owner surface.
    expect(read_status(g, v, fp("settings", sel_t::BARE), kOwner), kOk, "read :settings");
    expect(read_status(g, v, fp("settings", sel_t::BARE, {"app"}), kOwner), kOk,
           "read :settings.app");
    expect(read_status(g, v, fp("settings", sel_t::BARE, {"deadline_ns"}), kOwner),
           status_t::SCHEMA_NOT_FOUND, "read :settings.deadline_ns");
    expect(write_status(g, v, fp("settings", sel_t::BARE, {"deadline_ns"}), val, kOwner),
           status_t::SCHEMA_NOT_FOUND, "write :settings.deadline_ns");
    expect(write_status(g, v, fp("settings", sel_t::BARE, {"app"}), val, kOwner),
           status_t::SCHEMA_NOT_FOUND, "write :settings.app (container has no write surface)");
    expect(write_status(g, v, fp("settings", sel_t::BARE, {"app", "kp"}), val, kOwner), kOk,
           "write :settings.app.kp");
    expect(read_status(g, v, fp("settings", sel_t::BARE, {"app", "kp"}), kOwner), kOk,
           "read :settings.app.kp reads it back");
    expect(read_status(g, v, fp("settings", sel_t::BARE, {"app", "nope"}), kOwner),
           status_t::SCHEMA_NOT_FOUND, "read :settings.app.nope (undeclared)");
    expect(write_status(g, v, fp("settings", sel_t::BARE, {"app", "nope"}), val, kOwner),
           status_t::SCHEMA_NOT_FOUND, "write :settings.app.nope (undeclared)");

    // DIVERGENCE (#869) — `:settings[0].app.kp`. The read door tests `plain_step(steps[0])`
    // and the write door does not, so an INDEXED `:settings` selector writes the field and
    // then cannot be read back. Pinned, not fixed: tightening the write changes what leaves
    // the device for that spelling.
    field_path_t indexed_settings = fp("settings", sel_t::SLOT, {"app", "kp"});
    expect(write_status(g, v, indexed_settings, val, kOwner), kOk,
           "DIVERGENCE: write :settings[0].app.kp SUCCEEDS");
    expect(read_status(g, v, indexed_settings, kOwner), status_t::SCHEMA_NOT_FOUND,
           "DIVERGENCE: read :settings[0].app.kp is SCHEMA_NOT_FOUND");

    // `:subscribers` — the slot read; `[]` and bare have no single-view read surface here
    // (the whole-array read is `read_subscribers`, a separate entry point).
    expect(read_status(g, v, fp("subscribers", sel_t::SLOT), kOwner), status_t::NOT_FOUND,
           "read :subscribers[0] on an empty slot is NOT_FOUND");
    expect(read_status(g, v, fp("subscribers", sel_t::BARE), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :subscribers");
    expect(read_status(g, v, fp("subscribers", sel_t::SLOT, {"liveness"}), kOwner),
           status_t::SCHEMA_NOT_FOUND, "read :subscribers[0].liveness (#580)");
    expect(write_status(g, v, fp("subscribers", sel_t::BARE), val, kOwner),
           status_t::SCHEMA_NOT_FOUND, "write :subscribers");
    expect(write_status(g, v, fp("subscribers", sel_t::SLOT, {"liveness"}), val, kOwner),
           status_t::SCHEMA_NOT_FOUND, "write :subscribers[0].liveness (#580)");

    // An unknown field name is SCHEMA_NOT_FOUND on both doors, with no gate involved.
    expect(read_status(g, v, fp("status", sel_t::BARE), kOwner), status_t::SCHEMA_NOT_FOUND,
           "read :status (not in the namespace)");
    expect(write_status(g, v, fp("status", sel_t::BARE), val, kOwner), status_t::SCHEMA_NOT_FOUND,
           "write :status (not in the namespace)");
}

/**
 * @brief The pinned `:subscribers[*]` divergence (#869's headline).
 *
 * A `[*]` write is `INVALID_PATH` — RFC-0009/#579: the WRITE grammar has no wildcard axis and
 * the marker must never be silently resolved as `[0]`. The READ of the byte-identical
 * selector is `SCHEMA_NOT_FOUND`, and it is decided BELOW the READ gate, so a denied caller is
 * told `PERMISSION_DENIED` instead. Unifying the two is a wire question and needs an RFC:
 * docs/reference/03-addressing.md §`[*]` treated as `[0]` calls `[*]` "legal only where the
 * field chain's first step is subscribers" and reads it as "every slot", which points at an
 * enumerating READ rather than at either error code, while docs/reference/05 §0x09 permits
 * `SCHEMA_NOT_FOUND` "for a spelling it does not accept".
 */
void test_field_wildcard_divergence() {
    std::printf(":subscribers[*] — the read/write divergence, pinned:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sink_a"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sink_b"), role_t::STORED_VALUE);
    const field_path_t star = fp("subscribers", sel_t::WILDCARD);
    const field_path_t slot0 = fp("subscribers", sel_t::SLOT);

    // Slot 0 must be LIVE before the wildcard write. On an EMPTY slot vector an index-0
    // replace is refused OUT_OF_RANGE, which is ALSO `INVALID_PATH` — so a guard written
    // against an empty vertex passes even when the wildcard marker is ignored entirely.
    // (Measured: dropping the WILDCARD arm from `field_selector` leaves that weaker form
    // green.) Seeding the slot removes the accident and makes the row bite.
    expect(write_status(g, v, fp("subscribers", sel_t::APPEND), subscriber_tlv("sink_a"), {}), kOk,
           "seed: slot 0 holds a live subscriber to /sink_a");
    const auto seeded = g.read(v, slot0, {});
    check(seeded.has_value(), "seed reads back");
    const std::vector<std::byte> seeded_bytes =
        seeded
            ? std::vector<std::byte>(seeded->only().bytes().begin(), seeded->only().bytes().end())
            : std::vector<std::byte>{};

    expect(write_status(g, v, star, subscriber_tlv("sink_b"), {}), status_t::INVALID_PATH,
           "write :subscribers[*] is INVALID_PATH");
    expect(read_status(g, v, star, {}), status_t::SCHEMA_NOT_FOUND,
           "read :subscribers[*] is SCHEMA_NOT_FOUND — DIVERGES from the write");
    // The decisive half of #579: the LIVE slot 0 must be byte-identical afterwards — a
    // wildcard resolved as `[0]` would have replaced it with the /sink_b record.
    const auto after = g.read(v, slot0, {});
    check(after.has_value(), "slot 0 still reads back after the refused wildcard write");
    if (after) {
        const std::vector<std::byte> after_bytes(after->only().bytes().begin(),
                                                 after->only().bytes().end());
        check(!seeded_bytes.empty() && seeded_bytes == after_bytes,
              "... and it is BYTE-IDENTICAL — no wildcard write landed in slot 0 (#579)");
    }

    // The caller-dependence half: the write resolves the shape ABOVE its gate, the read
    // BELOW its own, so one spelling has two answers split by who is asking.
    graph_t gated;
    const vertex_handle_t gv = gated.register_vertex(path_t("/g"), role_t::STORED_VALUE);
    gated.set_subject_resolver(caller_is_subject);
    // A non-empty ACL that grants "peer-a" nothing at all (an empty effective ACL would be
    // unrestricted, so the ACE has to exist and name someone else).
    std::vector<tr::graph::ace_t> closed;
    closed.push_back(
        tr::graph::ace_t{.subject = as_bytes("peer-z"),
                         .access_mask = static_cast<std::uint32_t>(acl_right_t::READ)});
    (void)gated.write(gv, fp("acl", sel_t::BARE),
                      tr::view::rope_t{make_value(tr::graph::encode_acl(closed))}, {});
    expect(write_status(gated, gv, star, subscriber_tlv("sink"), "peer-a"), status_t::INVALID_PATH,
           "a DENIED caller's :subscribers[*] write is still INVALID_PATH (ungated)");
    expect(read_status(gated, gv, star, "peer-a"), status_t::PERMISSION_DENIED,
           "DIVERGENCE: a DENIED caller's :subscribers[*] read is PERMISSION_DENIED");
    expect(read_status(gated, gv, fp("acl", sel_t::SLOT), "peer-a"), status_t::PERMISSION_DENIED,
           "DIVERGENCE: and :acl[0] READ is PERMISSION_DENIED where the WRITE is "
           "SCHEMA_NOT_FOUND");
    expect(write_status(gated, gv, fp("acl", sel_t::SLOT), subscriber_tlv("sink"), "peer-a"),
           status_t::SCHEMA_NOT_FOUND, "... the write half of that same pair");
}

/**
 * @brief The three doors that admit a wire SUBSCRIBER share one parse — and keep their own
 *        pre- and post-conditions (#869 round 3).
 *
 * `parse_wire_subscriber` is the shared middle: type-check, parse, zero-copy retain. What is
 * NOT shared is what the doors disagree about — the `[N]` arm's WRITE gate and its eviction
 * sentinel run before it, the two `field_write` arms REQUIRE a `target_key` after it, and
 * `subscribe_wire` CLEARS that same key.
 */
void test_subscriber_door_parity() {
    std::printf("the three subscriber doors share one parse and keep their own conditions:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sink"), role_t::STORED_VALUE);
    // A POINT that carries the very PATH child a SUBSCRIBER would: only the TYPE code
    // separates it from an admissible record, so it isolates the shared type check.
    const tr::view::view_t junk = non_subscriber_with_path("sink");

    // (1) The shared type check — a non-SUBSCRIBER payload is TYPE_MISMATCH at all three.
    expect(write_status(g, v, fp("subscribers", sel_t::APPEND), junk, {}), status_t::TYPE_MISMATCH,
           "append arm: a non-SUBSCRIBER payload is TYPE_MISMATCH");
    expect(write_status(g, v, fp("subscribers", sel_t::SLOT), junk, {}), status_t::TYPE_MISMATCH,
           "[N] arm: a non-SUBSCRIBER payload is TYPE_MISMATCH");
    const auto wire_junk = g.subscribe_wire(v, junk, make_value({}), "link-a");
    expect(wire_junk ? kOk : outcome_t{wire_junk.error()}, status_t::TYPE_MISMATCH,
           "subscribe_wire: a non-SUBSCRIBER payload is TYPE_MISMATCH");

    // (2) The UNSHARED half. A SUBSCRIBER with no PATH child leaves `target_key` unset: the
    //     two field_write doors refuse it, and subscribe_wire — which resets the key anyway,
    //     because a wire consumer is named by the return route — accepts it.
    expect(write_status(g, v, fp("subscribers", sel_t::APPEND), subscriber_tlv_no_target(), {}),
           status_t::TYPE_MISMATCH, "append arm: a SUBSCRIBER with no target is TYPE_MISMATCH");
    expect(write_status(g, v, fp("subscribers", sel_t::SLOT), subscriber_tlv_no_target(), {}),
           status_t::TYPE_MISMATCH, "[N] arm: a SUBSCRIBER with no target is TYPE_MISMATCH");
    const auto wire_ok = g.subscribe_wire(v, subscriber_tlv_no_target(), make_value({}), "link-a");
    expect(wire_ok ? kOk : outcome_t{wire_ok.error()}, kOk,
           "subscribe_wire: the SAME record is ACCEPTED (it clears target_key itself)");

    // (3) The zero-copy retain the shared parse performs — the stored record reads back.
    graph_t g2;
    const vertex_handle_t v2 = g2.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    (void)g2.register_vertex(path_t("/sink"), role_t::STORED_VALUE);
    expect(write_status(g2, v2, fp("subscribers", sel_t::APPEND), subscriber_tlv("sink"), {}), kOk,
           "append arm: a well-formed SUBSCRIBER is admitted");
    const auto back = g2.read(v2, fp("subscribers", sel_t::SLOT), {});
    check(back.has_value(), "the retained record reads back from :subscribers[0]");
    if (back) {
        const auto decoded = tr::wire::decode(back->only());
        check(decoded && decoded->type == tr::wire::type_t::SUBSCRIBER,
              "... and it is the SUBSCRIBER TLV, verbatim");
    }
    // The [N] arm's own pre-condition, unaffected by the shared parse: the sentinel still
    // clears, and it is discriminated BEFORE the type check.
    expect(write_status(g2, v2, fp("subscribers", sel_t::SLOT),
                        make_value({0x09, 0x00, 0x00, 0x00}), {}),
           kOk, "[N] arm: the empty-STATUS sentinel still clears");
    expect(read_status(g2, v2, fp("subscribers", sel_t::SLOT), {}), status_t::NOT_FOUND,
           "... and the slot is empty afterwards");
}

/**
 * @brief A step carrying BOTH the append and the wildcard marker is refused, not silently
 *        treated as an append (#579's rule, applied once in `field_selector`).
 *
 * Neither producer of a `field_step_t` can build this: `path_t::parse` never sets `wildcard`,
 * and the wire decoder's `index_mode` ELEMENT and WILDCARD are exclusive switch arms. Only an
 * in-process caller hand-mutating a step reaches it — so this is an API-level answer, not a
 * wire one. Before #869 the write door tested `append` first and subscribed/created anyway.
 */
void test_append_plus_wildcard_is_refused() {
    std::printf("a hand-built append+wildcard step is refused, not resolved as an append:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sink"), role_t::STORED_VALUE);

    field_path_t both = fp("subscribers", sel_t::APPEND);
    both.steps[0].wildcard = true;
    expect(write_status(g, v, both, subscriber_tlv("sink"), {}), status_t::INVALID_PATH,
           "write :subscribers[]+[*] is INVALID_PATH (the wildcard wins)");
    expect(read_status(g, v, fp("subscribers", sel_t::SLOT), {}), status_t::NOT_FOUND,
           "... and nothing was admitted");

    field_path_t both_children = fp("children", sel_t::APPEND);
    both_children.steps[0].wildcard = true;
    expect(write_status(g, v, both_children, make_value({0x0e, 0x40, 0x00, 0x00}), {}),
           status_t::SCHEMA_NOT_FOUND, "write :children[]+[*] does not create");
    expect(read_status(g, v, both_children, {}), status_t::SCHEMA_NOT_FOUND,
           "read :children[]+[*] does not enumerate either");
}

}  // namespace

int main() {
    std::printf("field dispatch parity (#869)\n");
    test_field_shape_matrix();
    test_field_wildcard_divergence();
    test_subscriber_door_parity();
    test_append_plus_wildcard_is_refused();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
