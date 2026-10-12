/**
 * @file
 * @brief #1846 — the JOIN: the subject an in-band ACCEPT binds is the caller context of that
 *        session's later frames, so an `:acl` entry naming it gates them.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The REAL chip translation unit (`httpd_ws_link.cpp`) on the host fake of `esp_http_server`,
 * with a real `graph_t` and `fwd_router_t` behind it, because what is under test is a GATE
 * decision the graph makes: the link only answers which subject a frame writes under
 * (`transport_t::peer_subject`), and the terminus makes that answer the ACL caller context.
 *
 * What is pinned here:
 *   1. a session signed in as `role:operator` WRITEs a vertex whose `:acl` grants exactly that
 *      subject, and the handler sees the subject; the same WRITE from a session that signed in
 *      with no subject (so its caller is still its link name `p<slot>`) is DENIED;
 *   2. a later session in the same slot inherits nothing: its WRITE is denied;
 *   3. the same holds on a FLAT link (`peer_named = false`, ADR-0082);
 *   4. with no auth hook installed the caller is the link name, exactly as before.
 *
 * Ablation: making `httpd_ws_link_t::peer_subject` answer empty reddens 1 and 3; dropping its
 * generation test, or the reclaim's clear of the subject, reddens 2.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fake_httpd.hpp"
#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::write_ctx_t;
using tr::net::fwd_router_t;
using tr::net::httpd_ws_link_t;
using tr::testing::b_fwd;
using tr::testing::b_path;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief Drain the fake's control queue: the httpd task's own loop. */
void run_server() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A credential blob, opaque to the link. */
const std::byte kCredential[] = {std::byte{'t'}, std::byte{'o'}, std::byte{'k'}};

/** @brief The subject the scripted hook binds on its ACCEPT. */
std::string_view g_subject;

/** @brief An auth hook that ACCEPTs every credential and binds @ref g_subject. */
httpd_ws_link_t::auth_result_t accept_as(void*, int, std::span<const std::byte>) {
    httpd_ws_link_t::auth_result_t res;
    res.verdict = httpd_ws_link_t::auth_verdict_t::ACCEPT;
    res.subject = g_subject;
    return res;
}

/** @brief Every subject the `/knob` handler ran under, copied out of the call. */
std::vector<std::string> g_writers;

/** @brief The identity resolver: the caller context IS the subject token. */
std::expected<void, tr::wire::err_t> identity_lookup(void*, std::string_view caller,
                                                     tr::mem::bytes_t& out) {
    if (!out.append(reinterpret_cast<const std::byte*>(caller.data()), caller.size()))
        return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    return {};
}

/** @brief One ALLOW ACE granting @p subject WRITE, as a writable `:acl` value. */
tr::view::view_t grant_write(std::string_view subject) {
    std::vector<std::byte> token(subject.size());
    std::memcpy(token.data(), subject.data(), subject.size());
    const ace_t ace{.subject = token,
                    .access_mask = static_cast<std::uint32_t>(acl_right_t::WRITE)};
    const std::vector<std::byte> acl = tr::graph::encode_acl(std::span<const ace_t>(&ace, 1));
    return *tr::view::over_bytes(acl);
}

/**
 * @brief A graph with an ACL-enforced HANDLER at `/knob` that grants WRITE to
 *        `role:operator` only. @p enforce false leaves the resolver out (open by default).
 */
void build_graph(graph_t& g, bool enforce = true) {
    handlers_t h;
    h.on_write = {
        [](void*, const tr::graph::value_t&, const write_ctx_t& ctx) -> tr::graph::result_t<void> {
            g_writers.emplace_back(ctx.subject);
            return {};
        },
        nullptr};
    (void)g.register_vertex(*path_t::parse("/knob"), role_t::HANDLER, h);
    if (!enforce) return;
    auto hooks = g.hooks();
    hooks.subject_lookup = {identity_lookup, nullptr};
    g.set_hooks(hooks);
    (void)g.write(*path_t::parse("/knob:acl"), grant_write("role:operator"));
}

/** @brief One FWD{WRITE} of `/knob` — the request every case sends. */
std::vector<std::byte> write_knob() {
    const std::byte one[1] = {std::byte{0x42}};
    std::vector<std::byte> payload;
    tr::wire::emit_tlv(payload, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(one, 1));
    return b_fwd(fwd_op_t::WRITE, b_path({"knob"}), b_path({"origin"}), {}, payload);
}

/** @brief Open @p fd, sign it in under @p subject, and send one WRITE of `/knob`. */
void sign_in_and_write(int fd, std::string_view subject) {
    g_subject = subject;
    (void)fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kCredential);
    (void)fake_httpd::instance().deliver_frame(fd, write_knob());
    run_server();
}

/** @brief Start from a known-empty fake and an empty writer log. */
void reset() {
    fake_httpd::instance().close_all();
    run_server();
    fake_httpd::instance().clear_sent_frames();
    g_writers.clear();
}

/** @brief Case 1 and 2 at one `peer_named` setting. */
void signed_session_is_the_caller(bool peer_named) {
    std::printf("#1846 the bound subject is the caller context (peer_named=%d):\n",
                peer_named ? 1 : 0);
    reset();
    graph_t g;
    fwd_router_t router(g);
    build_graph(g);
    auto link =
        tr::mem::make_poly<httpd_ws_link_t>(tr::mem::net_source(), handle(), "/ws",
                                            tr::net::httpd_ws_config_t{.peer_named = peer_named});
    link->set_auth_cb(&accept_as, nullptr);
    check(router.add_child("ws", *link), "the link is a routed child");

    constexpr int kOperator = 810;
    constexpr int kAnonymous = 811;
    sign_in_and_write(kOperator, "role:operator");
    check(g_writers.size() == 1, "the operator's WRITE passed the :acl gate");
    if (g_writers.size() == 1)
        check(g_writers[0] == "role:operator", "and the handler saw the bound subject");

    sign_in_and_write(kAnonymous, "");
    check(g_writers.size() == 1, "the same WRITE from a session with no bound subject is DENIED");

    // Case 2: the operator leaves and a new session takes its slot without signing in as it.
    fake_httpd::instance().close_session(kOperator);
    run_server();
    sign_in_and_write(kOperator, "");
    check(g_writers.size() == 1, "the next session in the operator's slot inherits nothing");

    fake_httpd::instance().close_session(kOperator);
    fake_httpd::instance().close_session(kAnonymous);
    run_server();
}

/** @brief Case 4: no hook installed, the caller is the session's link name as before. */
void no_hook_keeps_the_link_name() {
    std::printf("#1846 with no auth hook the caller is the link name:\n");
    reset();
    graph_t g;
    fwd_router_t router(g);
    build_graph(g, /*enforce=*/false);
    auto link = tr::mem::make_poly<httpd_ws_link_t>(tr::mem::net_source(), handle(), "/ws",
                                                    tr::net::httpd_ws_config_t{.peer_named = true});
    check(router.add_child("ws", *link), "the link is a routed child");
    constexpr int kFd = 820;
    (void)fake_httpd::instance().open_session(kFd);
    (void)fake_httpd::instance().deliver_frame(kFd, write_knob());
    run_server();
    check(g_writers.size() == 1 && g_writers[0] == "p0",
          "the WRITE ran under the peer's link name p0, unchanged");
    fake_httpd::instance().close_session(kFd);
    run_server();
}

}  // namespace

int main() {
    std::printf("httpd_ws_link session subject suite (#1846):\n");
    signed_session_is_the_caller(/*peer_named=*/true);
    signed_session_is_the_caller(/*peer_named=*/false);
    no_hook_keeps_the_link_name();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
