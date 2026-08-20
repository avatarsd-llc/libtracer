/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — enforcement is opt-in TWICE, and the first ACE is what closes a vertex.
 *
 * A `graph_t` ships with no subject resolver, and a vertex ships with no `:acl`. Both of those
 * are open states, and they are INDEPENDENT: installing a resolver does not close a bare
 * vertex, and writing an `:acl` does not close a graph that cannot name its callers. Only the
 * two together enforce anything (ADR-0018, ADR-0020, #81).
 *
 * The rule at the far end is the one that surprises people: an effective ACL that is EMPTY is
 * open, but any present ACE closes the vertex to every subject that ACE does not name. There is
 * no "deny" to write — the first grant is the lock.
 *
 * The empty caller context is the third open state and the only one that is a convention rather
 * than a configuration: the in-process host API passes no caller, and that channel is trusted
 * without consulting the resolver at all (#905). A remote operation always carries its inbound
 * link NAME, so it cannot spell the trusted context.
 *
 * Runs under ctest as `example_acl_open_by_default`; returns non-zero on any failed check.
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

/** @brief @p s as opaque subject-token bytes — a subject is bytes, never a string (ADR-0018). */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The simplest resolver there is: the caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

/** @brief One ALLOW ACE granting @p subject exactly @p right, as a writable `:acl` value. */
tr::view::view_t one_grant(std::string_view subject, acl_right_t right) {
    const ace_t ace{.subject = as_bytes(subject), .access_mask = static_cast<std::uint32_t>(right)};
    const std::vector<std::byte> acl = tr::graph::encode_acl(std::span<const ace_t>(&ace, 1));
    return *tr::view::over_bytes(acl);
}

/** @brief A one-byte VALUE — the payload every write below carries. */
tr::view::view_t some_value() {
    const std::byte one[1] = {std::byte{0x01}};
    return *tr::view::over_bytes(one);
}

}  // namespace

int main() {
    bool ok = true;

    // 1. No resolver. The ACL is STORED and enforced by nobody: the graph cannot turn a caller
    //    context into a subject, so there is nothing to match an ACE against.
    {
        graph_t g;
        const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
        (void)g.write(v, some_value());  // seed a value, so a refused READ is the only failure
        check(ok, g.write(path_t("/x:acl"), one_grant("peer-z", acl_right_t::READ)).has_value(),
              "the :acl is accepted and stored with no resolver installed");
        check(ok, g.read(v, "peer-a").has_value(),
              "…and peer-a still reads: with no resolver, enforcement is off entirely");
    }

    // 2. A resolver, and a vertex nobody wrote an ACE to. Open, per vertex.
    {
        graph_t g;
        g.configure_subject_resolver(caller_is_subject, nullptr);
        const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
        check(ok, g.write(v, some_value(), "peer-a").has_value(),
              "a resolver alone enforces nothing — a vertex with no ACE is open");
    }

    // 3. Both. Now the single ACE is the lock: it grants peer-z READ and, by existing at all,
    //    refuses everyone else — including for rights it never mentions.
    {
        graph_t g;
        g.configure_subject_resolver(caller_is_subject, nullptr);
        const vertex_handle_t v = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
        (void)g.write(v, some_value());  // seed a value as the trusted local caller
        (void)g.write(path_t("/x:acl"), one_grant("peer-z", acl_right_t::READ));

        check(ok, g.read(v, "peer-z").has_value(), "peer-z reads — the ACE names it");
        const auto denied = g.read(v, "peer-a");
        check(ok, !denied && denied.error() == status_t::PERMISSION_DENIED,
              "peer-a is refused, and no ACE ever said so — presence is what closes");
        const auto no_write = g.write(v, some_value(), "peer-z");
        check(ok, !no_write && no_write.error() == status_t::PERMISSION_DENIED,
              "even peer-z cannot WRITE: the grant is one bit, not a door");

        // 4. And the local host API is still trusted, which is how the ACL got written above.
        check(ok, g.write(v, some_value()).has_value(),
              "the empty caller context is the trusted local channel (#905)");
    }

    std::printf("open by default: no resolver, no ACE, or no caller — any one of them opens\n");
    return ok ? 0 : 1;
}
