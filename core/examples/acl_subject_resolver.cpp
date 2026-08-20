/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the caller context is not the subject; a RESOLVER maps one to the other.
 *
 * An ACE names a SUBJECT TOKEN — opaque bytes. An operation arrives carrying a CALLER CONTEXT —
 * this node's own NAME for the inbound link the frame came in on. Those are different things,
 * and the pluggable `subject_resolver_fn_t` is the seam between them (ADR-0018): it is where an
 * integrator turns "the frame came in on link `ws:7`" into "and that link belongs to `alice`".
 * Authorization is what libtracer does; deciding whose identity a link carries is the
 * resolver's, which is why v1's transport-authenticated peer id and a later ed25519 key both
 * fit the same ACL model without changing it.
 *
 * Three properties of the seam that a resolver author has to get right, all shown below:
 *
 *  - The ERROR arm is a **deny**, not a fallback (#905). "I cannot name this caller" refuses at
 *    every gate. Its predecessor returned `std::optional`, whose `nullopt` meant FULLY TRUSTED —
 *    so an unresolvable caller used to be granted everything, `WRITE_ACL` included.
 *  - The EMPTY caller context never reaches the resolver at all. The graph settles it as the
 *    trusted local channel first, so the resolver is never asked to have an opinion about it.
 *  - It is a `{fn, ctx}` pair, not a `std::function` (#1049): whatever state the resolver needs
 *    travels in a caller-owned `ctx` that must outlive every gated operation.
 *
 * Runs under ctest as `example_acl_subject_resolver`; returns non-zero on any failed check.
 */

#include <cstdio>
#include <cstring>
#include <expected>
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

/** @brief @p s as opaque token bytes. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The resolver's own state — caller-owned, and the reason `ctx` exists. */
struct link_directory_t {
    /** @brief How many times the gate invoked the resolver — the empty caller never does. */
    int invocations = 0;
};

/**
 * @brief A resolver with a real mapping in it: link name in, principal out.
 *
 * `ws:7` is a link this node accepted; `alice` is who is behind it. An ACE names `alice`,
 * because a principal outlives the link it happens to be dialled in on today.
 */
std::expected<subject_token_t, tr::wire::err_t> resolve_link_owner(void* ctx,
                                                                   std::string_view caller) {
    static_cast<link_directory_t*>(ctx)->invocations += 1;
    if (caller == "ws:7") return as_bytes("alice");
    if (caller == "ws:8") return as_bytes("bob");
    // Every other link is one this node cannot put a name to: a stale link, a revoked peer,
    // a lookup that failed. That is a DENY, and saying so is the whole point of the arm.
    return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
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

/** @brief True iff @p r was refused by an ACL gate. */
template <class T>
bool denied(const tr::graph::result_t<T>& r) {
    return !r.has_value() && r.error() == status_t::PERMISSION_DENIED;
}

}  // namespace

int main() {
    bool ok = true;
    link_directory_t directory;

    graph_t g;
    g.configure_subject_resolver(resolve_link_owner, &directory);
    const vertex_handle_t v = g.register_vertex(path_t("/dev/temp"), role_t::STORED_VALUE);
    (void)g.write(v, some_value());  // the trusted local caller seeds a value…
    (void)g.write(path_t("/dev/temp:acl"), one_grant("alice", acl_right_t::READ));  // …and the ACL

    check(ok, directory.invocations == 0,
          "two local writes, zero resolver calls — the empty context is settled first (#905)");

    // The ACE says `alice`. Nothing anywhere says `ws:7`; the resolver is the only thing that
    // knows the two are connected, and it is an integrator's code, not the library's.
    check(ok, g.read(v, "ws:7").has_value(), "link ws:7 resolves to alice, who is granted READ");
    check(ok, denied(g.read(v, "ws:8")), "link ws:8 resolves to bob, who is not");
    check(ok, denied(g.read(v, "ws:9")),
          "link ws:9 resolves to NOBODY — the error arm denies, it does not wave through");
    check(ok, directory.invocations == 3, "three remote reads, three resolver calls");

    // The deny arm is a deny at EVERY gate, control plane included — which is exactly what the
    // std::optional predecessor got wrong: an unnameable caller could rewrite the ACL.
    check(ok, denied(g.write(v, some_value(), "ws:9")), "…and the unnameable caller cannot WRITE");
    check(ok,
          denied(g.write(v, path_t::parse("/dev/temp:acl")->field(),
                         one_grant("ws:9", acl_right_t::READ), "ws:9")),
          "…and above all cannot rewrite the :acl to grant itself the access");

    std::printf("resolver invoked %d times; ws:7=alice, ws:8=bob, everything else denied\n",
                directory.invocations);
    return ok ? 0 : 1;
}
