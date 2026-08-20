/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a vertex's EFFECTIVE ACL is its own ACEs plus the INHERIT-flagged
 *        ancestor ones.
 *
 * `:acl` is not written per leaf, the way `:subscribers[]` is not: an ACE on a composite carries
 * the `kAceInherit` flag and applies to that composite's whole subtree, so an owner grants an
 * orchestrator admin over `/dev` once instead of over every endpoint below it (ADR-0020, NFSv4
 * inheritance riding the address composition).
 *
 * Inheritance is PER ACE, not per list. One `:acl` can hold both kinds side by side, and the
 * unflagged ones apply to the vertex they were written to and nowhere else. That asymmetry is
 * the whole example, and it produces the one result worth internalising: a descendant whose only
 * candidate ACE is an ancestor's UNFLAGGED one has an EMPTY effective ACL, and an empty effective
 * ACL is open — so the parent is closed while the child beneath it is not.
 *
 * Nothing here is cached as a verdict. An `:acl` write marks the subtree dirty and the next
 * check rebuilds the merge, so a revoked grant takes effect on the very next operation.
 *
 * Runs under ctest as `example_acl_inherit`; returns non-zero on any failed check.
 */

#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/security_acl.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::kAceInherit;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;

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

/** @brief The caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

/** @brief @p right as the single `access_mask` bit it is. */
constexpr std::uint32_t bit(acl_right_t right) { return static_cast<std::uint32_t>(right); }

/** @brief A one-byte VALUE. */
tr::view::view_t some_value() {
    const std::byte one[1] = {std::byte{0x01}};
    return *tr::view::over_bytes(one);
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

    const vertex_handle_t dev = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    const vertex_handle_t temp = g.register_vertex(path_t("/dev/temp"), role_t::STORED_VALUE);
    const vertex_handle_t raw = g.register_vertex(path_t("/dev/temp/raw"), role_t::STORED_VALUE);
    for (vertex_handle_t v : {dev, temp, raw}) (void)g.write(v, some_value());  // trusted seeds

    // One :acl on the composite, carrying both kinds of ACE.
    const ace_t composite[] = {
        {.flags = kAceInherit, .subject = as_bytes("fleet"), .access_mask = bit(acl_right_t::READ)},
        {.subject = as_bytes("local"), .access_mask = bit(acl_right_t::WRITE)},
    };
    (void)g.write(path_t("/dev:acl"), *tr::view::over_bytes(tr::graph::encode_acl(composite)));

    // The flagged ACE reaches the whole subtree, at any depth.
    check(ok, g.read(temp, "fleet").has_value(), "the INHERIT ACE grants READ one level down");
    check(ok, g.read(raw, "fleet").has_value(), "…and two: it covers the subtree, not the child");
    check(ok, denied(g.write(temp, some_value(), "fleet")),
          "…and it inherits ONE bit — an inherited READ is not a WRITE");

    // The unflagged one applies where it was written, and stops there.
    check(ok, g.write(dev, some_value(), "local").has_value(),
          "the unflagged ACE grants WRITE on /dev itself");
    check(ok, denied(g.write(temp, some_value(), "local")),
          "…and does not travel: /dev/temp is closed by the inherited ACE, which does not name it");

    // Own ACEs come first, and combine with what is inherited rather than replacing it.
    const ace_t own[] = {{.subject = as_bytes("app"), .access_mask = bit(acl_right_t::WRITE)}};
    (void)g.write(path_t("/dev/temp:acl"), *tr::view::over_bytes(tr::graph::encode_acl(own)));
    check(ok, g.write(temp, some_value(), "app").has_value(), "the child's own ACE grants WRITE");
    check(ok, g.read(temp, "fleet").has_value(),
          "…and the inherited READ still applies alongside it");

    // The consequence: an unflagged ancestor ACE leaves its descendants with NOTHING to
    // evaluate, and nothing to evaluate is open. A closed parent over an open child.
    const vertex_handle_t site = g.register_vertex(path_t("/site"), role_t::STORED_VALUE);
    const vertex_handle_t leaf = g.register_vertex(path_t("/site/leaf"), role_t::STORED_VALUE);
    (void)g.write(site, some_value());
    (void)g.write(leaf, some_value());
    const ace_t unflagged[] = {
        {.subject = as_bytes("someone"), .access_mask = bit(acl_right_t::READ)}};
    (void)g.write(path_t("/site:acl"), *tr::view::over_bytes(tr::graph::encode_acl(unflagged)));
    check(ok, denied(g.write(site, some_value(), "stranger")),
          "the ACE closes /site to a stranger");
    check(ok, g.write(leaf, some_value(), "stranger").has_value(),
          "…and /site/leaf stays OPEN: an unflagged ancestor ACE is not in its effective ACL");

    std::printf("effective ACL = own ACEs + ancestor ACEs carrying flag 0x%02x\n", kAceInherit);
    return ok ? 0 : 1;
}
