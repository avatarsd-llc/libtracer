/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `access_mask` holds two rights, and a `:` field is a path under its
 *        vertex (RFC-0036).
 *
 * `acl_right_t` has two single bits, READ and WRITE, and every gate in the graph asks for one
 * of them (docs/reference/05 §0x0A). A `:` field has no ACL of its own: reading any field of a
 * vertex needs READ on it, and writing one needs WRITE. Subscribing is a read, because a
 * subscription delivers what a read would; creating a child is a write to the parent.
 *
 * Two things are not rights at all. Writing `:acl` is the OWNER's, the empty local caller's,
 * and no ACE can grant it, so a subject holding both bits still cannot rewrite the policy. A
 * deployment that wants remote administration registers an owner-app vertex whose handler
 * applies ACEs by the application's own policy (reference/13). And a `:subscribers[N]` slot is
 * its holder's: a subject with READ may clear its own, never another's.
 *
 * A mask bit outside READ|WRITE is refused at install with `TYPE_MISMATCH`, so a retired right
 * is never stored and left inert.
 *
 * Runs under ctest as `example_acl_right_bits`; returns non-zero on any failed check.
 */

#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief @p s as opaque subject-token bytes. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief Append @p subject to the gate's buffer @p out; a refused append fails closed. */
std::expected<void, tr::wire::err_t> put_subject(tr::mem::bytes_t& out, std::string_view subject) {
    if (!out.append(reinterpret_cast<const std::byte*>(subject.data()), subject.size()))
        return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    return {};
}

/** @brief The caller context IS the subject token — the resolver is not what this example is
 *         about (see acl_subject_resolver for the seam itself). */
std::expected<void, tr::wire::err_t> caller_is_subject(void*, std::string_view caller,
                                                       tr::mem::bytes_t& out) {
    return put_subject(out, caller);
}

/** @brief @p right as the single `access_mask` bit it is. */
constexpr std::uint32_t bit(acl_right_t right) { return static_cast<std::uint32_t>(right); }

/** @brief A one-byte VALUE — the payload the WRITE gate carries. */
tr::view::view_t some_value() {
    const std::byte one[1] = {std::byte{0x01}};
    return *tr::view::over_bytes(one);
}

/** @brief A `SUBSCRIBER{PATH}` naming @p target — the value a `:subscribers[]` append carries. */
tr::view::view_t subscriber_to(std::string_view target) {
    std::vector<std::byte> path;
    (void)tr::wire::emit_path_segment(path, target);
    std::vector<std::byte> path_tlv;
    tr::wire::emit_tlv(path_tlv, type_t::PATH, opt_t{}, path);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, path_tlv);
    return *tr::view::over_bytes(out);
}

/** @brief A `SPEC` creating a STORED_VALUE child named @p name — the `:children[]` value. */
tr::view::view_t child_named(std::string_view name) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, "stored_value");
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return *tr::view::over_bytes(out);
}

/** @brief True iff @p r was refused by an ACL gate. */
template <class T>
bool denied(const tr::graph::result_t<T>& r) {
    return !r.has_value() && r.error() == status_t::PERMISSION_DENIED;
}

}  // namespace

int main() {
    bool ok = true;
    graph_t g;
    g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {caller_is_subject, nullptr});
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, some_value());  // trusted local seed, so a refused read is the only failure

    // One ACE per right, one subject per ACE, and one subject holding both.
    const ace_t aces[] = {
        {.subject = as_bytes("reader"), .access_mask = bit(acl_right_t::READ)},
        {.subject = as_bytes("writer"), .access_mask = bit(acl_right_t::WRITE)},
        {.subject = as_bytes("both"),
         .access_mask = bit(acl_right_t::READ) | bit(acl_right_t::WRITE)},
    };
    (void)g.write(path_t("/x:acl"), *tr::view::over_bytes(tr::graph::encode_acl(aces)));

    const auto acl_field = path_t::parse("/x:acl");
    const auto subscribers = path_t::parse("/x:subscribers[]");
    const auto children = path_t::parse("/x:children[]");

    // READ (0x01) — the read door, every `:` field read, and subscribe.
    check(ok, g.read(v, "reader").has_value(), "READ: the reader reads");
    check(ok, denied(g.read(v, "writer")), "READ: the writer may not — WRITE is a different bit");
    check(ok, g.read(v, acl_field->field(), "reader").has_value(),
          "READ: the reader reads the :acl, a field like any other");
    check(ok, g.write(v, subscribers->field(), subscriber_to("sink"), "reader").has_value(),
          "READ: the reader subscribes — a subscription delivers what a read would");
    check(ok, denied(g.write(v, subscribers->field(), subscriber_to("sink"), "writer")),
          "READ: a WRITE grant does not buy a subscription");

    // WRITE (0x02) — the write door, every `:` field write, creation, and fan-in deliveries.
    check(ok, g.write(v, some_value(), "writer").has_value(), "WRITE: the writer writes");
    check(ok, denied(g.write(v, some_value(), "reader")), "WRITE: the reader may not");
    check(ok, g.write(v, children->field(), child_named("kid"), "writer").has_value(),
          "WRITE: the writer creates /x/kid — creating is a write to the parent");
    check(ok, denied(g.write(v, children->field(), child_named("other"), "reader")),
          "WRITE: the reader may not create a child");

    // `:acl` writes are the owner's: no ACE grants them, both bits included.
    const ace_t next[] = {{.subject = as_bytes("both"), .access_mask = bit(acl_right_t::READ)}};
    const tr::view::view_t next_acl = *tr::view::over_bytes(tr::graph::encode_acl(next));
    check(ok, denied(g.write(v, acl_field->field(), next_acl, "both")),
          ":acl: a remote subject holding READ|WRITE cannot rewrite the policy");
    check(ok, g.write(*acl_field, next_acl).has_value(), ":acl: the owner can");

    // A retired right is refused at install, and nothing is stored.
    const ace_t retired[] = {{.subject = as_bytes("watcher"), .access_mask = 0x04}};
    const auto refused = g.write(*acl_field, *tr::view::over_bytes(tr::graph::encode_acl(retired)));
    check(ok, !refused.has_value() && refused.error() == status_t::TYPE_MISMATCH,
          "a mask bit outside READ|WRITE is refused with TYPE_MISMATCH");

    std::printf("two rights, two single-bit grants: 0x%02x 0x%02x\n", bit(acl_right_t::READ),
                bit(acl_right_t::WRITE));
    return ok ? 0 : 1;
}
