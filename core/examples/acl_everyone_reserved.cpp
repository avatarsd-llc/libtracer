/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `EVERYONE@` is the one wildcard subject, and it is RESERVED both ways.
 *
 * The subject-token space is opaque bytes, with exactly one spelling the evaluators know:
 * `tr::graph::kEveryoneSubject` — `"EVERYONE@"`. An ACE carrying it applies to every resolved
 * subject, which is how a vertex is opened to all comers for one right without enumerating
 * anybody (ADR-0020).
 *
 * The half that is easy to miss is the OTHER direction. The wire has ONE spelling for a subject
 * token: the ACE's subject and the resolver's output are the same opaque bytes. So a principal
 * that could BE those bytes would be indistinguishable from the wildcard — and the deployments
 * at risk are exactly the ones whose resolver passes a caller-supplied identity through
 * (usernames, certificate CNs, peer names). The core therefore refuses to let a RESOLVED subject
 * spell it (#908): that caller is denied at every gate, on a guarded vertex and on a bare one
 * alike, rather than every integrator being left to know to blacklist the string.
 *
 * Note what the bare-vertex case means: the refusal happens BEFORE the open-by-default rule, so
 * it is not "the wildcard subject matches no ACE" — it is "this is not a principal at all".
 *
 * `OWNER@` is deliberately absent. ADR-0020 named it once, no evaluator ever special-cased it,
 * and so an `OWNER@` ACE matched nobody while still CLOSING the vertex it was written to
 * delegate — the erratum (#1033) withdraws the name rather than reserving it.
 *
 * Runs under ctest as `example_acl_everyone_reserved`; returns non-zero on any failed check.
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
using tr::graph::kEveryoneSubject;
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

/**
 * @brief The pass-through resolver — the shape #908 exists for.
 *
 * It hands back whatever the caller context said, which is what an integrator writes when the
 * transport already authenticated a name. It is also exactly what would let a peer that managed
 * to be called `EVERYONE@` resolve to the wildcard, so the core refuses the OUTPUT rather than
 * trusting every such resolver to blacklist the token itself.
 */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

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
    const vertex_handle_t guarded = g.register_vertex(path_t("/guarded"), role_t::STORED_VALUE);
    const vertex_handle_t bare = g.register_vertex(path_t("/bare"), role_t::STORED_VALUE);
    (void)g.write(guarded, some_value());  // trusted local seeds, so a refused READ is the
    (void)g.write(bare, some_value());     // only way either read below can fail

    // The wildcard ACE: READ for everybody, and nothing else for anybody.
    const ace_t wildcard[] = {{.subject = as_bytes(kEveryoneSubject),
                               .access_mask = static_cast<std::uint32_t>(acl_right_t::READ)}};
    (void)g.write(path_t("/guarded:acl"), *tr::view::over_bytes(tr::graph::encode_acl(wildcard)));

    // Direction 1 — as an ACE subject, it matches every principal, named or not.
    check(ok, g.read(guarded, "alice").has_value(), "the wildcard ACE grants READ to alice");
    check(ok, g.read(guarded, "a-peer-nobody-enumerated").has_value(), "…and to a stranger");
    check(ok, denied(g.write(guarded, some_value(), "alice")),
          "…and it is still ONE bit: the wildcard granted READ, not access");

    // Direction 2 — as a RESOLVED subject, it is not a principal at all.
    check(ok, denied(g.read(guarded, kEveryoneSubject)),
          "a caller resolving to EVERYONE@ is denied on the guarded vertex…");
    check(ok, denied(g.read(bare, kEveryoneSubject)),
          "…and on the BARE one too, which open-by-default would otherwise have allowed");
    check(ok, g.read(bare, "alice").has_value(),
          "the ablation: the bare vertex really is open to an ordinary caller");

    // The same predicate, published so a resolver can refuse the token at its own door as well.
    check(ok, tr::graph::is_reserved_subject(as_bytes(kEveryoneSubject)),
          "is_reserved_subject() is the check, and it is public for that reason");
    check(ok, !tr::graph::is_reserved_subject(as_bytes("OWNER@")),
          "OWNER@ is NOT reserved and NOT special — it is an ordinary opaque token (#1033)");

    std::printf("one reserved subject, %zu bytes: \"%.*s\"\n", kEveryoneSubject.size(),
                static_cast<int>(kEveryoneSubject.size()), kEveryoneSubject.data());
    return ok ? 0 : 1;
}
