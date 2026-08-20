/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `access_mask` is a bitfield, and every gate tests exactly ONE bit.
 *
 * There is no "access level" and no ordering among the rights: `acl_right_t` values are single
 * bits, a stored ACE may carry any OR of them, and each door in the graph asks for its own bit
 * and no other (docs/reference/05 §0x0A, ADR-0020). A subject granted `READ` cannot write; a
 * subject granted `READ` cannot even read the `:acl`, because reading the policy is
 * `READ_ACL` — its own right.
 *
 * The bit worth naming is `WRITE_ACL`: it is PRECISELY the admin right, the one that lets a
 * subject rewrite the policy and so delegate to others. Not a role, not a flag beside the mask —
 * one bit in the same bitfield, which is what makes "the owner grants an orchestrator admin over
 * a subtree, and the orchestrator leaves" an ordinary ACE write (reference/13).
 *
 * Six subjects, six single-bit grants, one vertex — so every allow below is the bit under test
 * and every deny is a subject that holds a DIFFERENT bit rather than nothing at all.
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
using tr::graph::subject_token_t;
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

/** @brief The caller context IS the subject token — the resolver is not what this example is
 *         about (see acl_subject_resolver for the seam itself). */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
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
    g.configure_subject_resolver(caller_is_subject, nullptr);
    const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.write(v, some_value());  // trusted local seed, so a refused read is the only failure

    // One ACE per right, one subject per ACE. No subject holds two bits.
    const ace_t aces[] = {
        {.subject = as_bytes("reader"), .access_mask = bit(acl_right_t::READ)},
        {.subject = as_bytes("writer"), .access_mask = bit(acl_right_t::WRITE)},
        {.subject = as_bytes("watcher"), .access_mask = bit(acl_right_t::SUBSCRIBE)},
        {.subject = as_bytes("builder"), .access_mask = bit(acl_right_t::CREATE)},
        {.subject = as_bytes("auditor"), .access_mask = bit(acl_right_t::READ_ACL)},
        {.subject = as_bytes("admin"), .access_mask = bit(acl_right_t::WRITE_ACL)},
    };
    (void)g.write(path_t("/x:acl"), *tr::view::over_bytes(tr::graph::encode_acl(aces)));

    const auto acl_field = path_t::parse("/x:acl");
    const auto subscribers = path_t::parse("/x:subscribers[]");
    const auto children = path_t::parse("/x:children[]");

    // READ (0x01) — the data plane's read door.
    check(ok, g.read(v, "reader").has_value(), "READ: the reader reads");
    check(ok, denied(g.read(v, "writer")), "READ: the writer may not — WRITE is a different bit");

    // WRITE (0x02) — the data plane's write door, and the fan-in gate for deliveries.
    check(ok, g.write(v, some_value(), "writer").has_value(), "WRITE: the writer writes");
    check(ok, denied(g.write(v, some_value(), "reader")), "WRITE: the reader may not");

    // SUBSCRIBE (0x04) — the PRODUCER's fan-out gate: who may append to my :subscribers[].
    check(ok, g.write(v, subscribers->field(), subscriber_to("sink"), "watcher").has_value(),
          "SUBSCRIBE: the watcher appends a :subscribers[] edge");
    check(ok, denied(g.write(v, subscribers->field(), subscriber_to("sink"), "writer")),
          "SUBSCRIBE: a WRITE grant does not buy a subscription");

    // CREATE (0x08) — in-band vertex creation is a write, and it has its own bit (ADR-0017).
    check(ok, g.write(v, children->field(), child_named("kid"), "builder").has_value(),
          "CREATE: the builder creates /x/kid");
    check(ok, denied(g.write(v, children->field(), child_named("other"), "writer")),
          "CREATE: the writer may not create a child");

    // READ_ACL (0x20) — reading the policy is not reading the value.
    check(ok, g.read(v, acl_field->field(), "auditor").has_value(),
          "READ_ACL: the auditor reads the :acl");
    check(ok, denied(g.read(v, acl_field->field(), "reader")),
          "READ_ACL: READ alone does not disclose the policy");

    // WRITE_ACL (0x40) — precisely the admin right: rewrite the policy, delegate to others.
    const ace_t delegated[] = {
        {.subject = as_bytes("admin"), .access_mask = bit(acl_right_t::WRITE_ACL)}};
    check(ok,
          g.write(v, acl_field->field(), *tr::view::over_bytes(tr::graph::encode_acl(delegated)),
                  "admin")
              .has_value(),
          "WRITE_ACL: the admin rewrites the policy — this bit IS 'admin'");
    check(ok,
          denied(g.write(v, acl_field->field(),
                         *tr::view::over_bytes(tr::graph::encode_acl(delegated)), "writer")),
          "WRITE_ACL: nobody else can, however much of the data plane they hold");

    std::printf("six rights, six single-bit grants: 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
                bit(acl_right_t::READ), bit(acl_right_t::WRITE), bit(acl_right_t::SUBSCRIBE),
                bit(acl_right_t::CREATE), bit(acl_right_t::READ_ACL), bit(acl_right_t::WRITE_ACL));
    return ok ? 0 : 1;
}
