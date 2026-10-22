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
 *   4. with no auth hook installed the caller is the link name, exactly as before;
 *   5. an ACCEPT whose subject is longer than the field or carries a NUL binds nothing, is
 *      counted in `auth_rejected`, and closes the session like a REJECT;
 *   6. a `:subscribers[N]` edge a signed-in session admitted leaves with THAT session (#2135):
 *      two sessions bound to the same subject each replace one slot, and each departure
 *      evicts its own edge and leaves the other's delivering.
 *
 * Ablation: making `httpd_ws_link_t::peer_subject` answer empty reddens 1 and 3; dropping its
 * generation test, or the reclaim's clear of the subject, reddens 2. Keying the `[N]` edge's
 * departure on its subject again (`write_ctx_t::session` ignored) reddens 6.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
    g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {identity_lookup, nullptr});
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
    check(router.attach_link("ws", *link), "the link is a routed child");

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

/**
 * @brief Case 5: a subject the session cannot store whole binds nothing. One longer than the
 *        field whose first 127 bytes are a privileged subject, and one that is a privileged
 *        subject followed by a NUL and more bytes: each ACCEPT is answered as a REJECT and
 *        counted, and neither session's WRITE gets the privileged subject's access.
 */
void unfit_subject_is_refused() {
    std::printf("#1846 a subject that does not fit, or carries a NUL, is refused:\n");
    reset();
    graph_t g;
    fwd_router_t router(g);
    build_graph(g);
    auto link = tr::mem::make_poly<httpd_ws_link_t>(tr::mem::net_source(), handle(), "/ws",
                                                    tr::net::httpd_ws_config_t{.peer_named = true});
    link->set_auth_cb(&accept_as, nullptr);
    check(router.attach_link("ws", *link), "the link is a routed child");
    // An :acl that grants the exact 127-byte prefix, so a shortened subject WOULD match it.
    const std::string prefix =
        "role:operator" + std::string(httpd_ws_link_t::kMaxSubjectChars - 13, 'x');
    (void)g.write(*path_t::parse("/knob:acl"), grant_write(prefix));

    const std::string too_long = prefix + "y";
    sign_in_and_write(830, too_long);
    check(g_writers.empty(), "the over-long subject's WRITE gains nothing");
    check(link->stats().auth_rejected == 1, "and its ACCEPT was counted as a refused sign-in");
    check(!fake_httpd::instance().has_session(830), "and the session was closed");

    (void)g.write(*path_t::parse("/knob:acl"), grant_write("role:operator"));
    const char with_nul[] = {'r', 'o', 'l', 'e', ':', 'o',  'p', 'e',
                             'r', 'a', 't', 'o', 'r', '\0', 'x'};
    sign_in_and_write(831, std::string_view(with_nul, sizeof(with_nul)));
    check(g_writers.empty(), "the subject with an embedded NUL gains nothing");
    check(link->stats().auth_rejected == 2, "and its ACCEPT was counted too");
    check(!fake_httpd::instance().has_session(831), "and that session was closed");

    // The field's exact capacity still binds: the rule is whole-or-nothing, not shorter.
    (void)g.write(*path_t::parse("/knob:acl"), grant_write(prefix));
    sign_in_and_write(832, prefix);
    check(g_writers.size() == 1 && g_writers[0] == prefix,
          "a subject of exactly kMaxSubjectChars binds and is granted");
    fake_httpd::instance().close_session(832);
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
    check(router.attach_link("ws", *link), "the link is a routed child");
    constexpr int kFd = 820;
    (void)fake_httpd::instance().open_session(kFd);
    (void)fake_httpd::instance().deliver_frame(kFd, write_knob());
    run_server();
    check(g_writers.size() == 1 && g_writers[0] == "p0~1",
          "the WRITE ran under the peer's session name p0~1, unchanged");
    fake_httpd::instance().close_session(kFd);
    run_server();
}

/** @brief Every `/a`, `/b` and `/c` delivery the `:subscribers[N]` case observed, by vertex name.
 */
std::vector<std::string> g_delivered;

/** @brief A HANDLER that records which target a fan-out delivery reached. */
handlers_t record_as(const char* name) {
    handlers_t h;
    h.on_write = {
        [](void* ctx, const tr::graph::value_t&, const write_ctx_t&) -> tr::graph::result_t<void> {
            g_delivered.emplace_back(static_cast<const char*>(ctx));
            return {};
        },
        const_cast<char*>(name)};
    return h;
}

/** @brief SUBSCRIBER{ PATH /@p target } — a local fan-out target (RFC-0009 §D.1 replace). */
std::vector<std::byte> subscriber_to(std::string_view target) {
    const std::vector<std::byte> path = b_path({target});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, path);
    return out;
}

/** @brief FIELD{ NAME subscribers, VALUE u32 @p slot, VALUE u8 ELEMENT } — `:subscribers[N]`. */
std::vector<std::byte> subscribers_slot(std::uint32_t slot) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "subscribers");
    const std::byte idx[4] = {std::byte(slot & 0xFFu), std::byte((slot >> 8) & 0xFFu),
                              std::byte((slot >> 16) & 0xFFu), std::byte((slot >> 24) & 0xFFu)};
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(idx, 4));
    const std::byte element{1};  // index_mode = ELEMENT
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(&element, 1));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::FIELD, tr::wire::opt_t{.pl = true}, body);
    return out;
}

/** @brief Publish to `/src` as the owner and answer which targets the fan-out reached. */
std::vector<std::string> publish_src(graph_t& g) {
    g_delivered.clear();
    const std::byte one[1] = {std::byte{0x07}};
    std::vector<std::byte> value;
    tr::wire::emit_tlv(value, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(one, 1));
    (void)g.write(*path_t::parse("/src"), *tr::view::over_bytes(value));
    return g_delivered;
}

/** @brief True iff @p got holds @p name. */
bool reached(const std::vector<std::string>& got, std::string_view name) {
    for (const std::string& s : got)
        if (s == name) return true;
    return false;
}

/**
 * @brief Case 6 (#2135): an edge a signed-in session admits through `:subscribers[N]` is
 *        evicted when that session leaves, and two sessions bound to the SAME subject never
 *        share that eviction.
 *
 * Two local edges on `/src` (slots 0 and 1) are seeded under the subject `role:operator`, so
 * that subject holds them (RFC-0036 §3.4: an edge the owner wires is the owner's alone). Two
 * sessions sign in as `role:operator`; each replaces one slot over the wire with an edge to its
 * own target. The
 * replace is the field-write door's (RFC-0009 §D.1): it binds no delivery link, so what its
 * session's departure finds it by is the only thing under test.
 */
void slot_edge_leaves_with_its_session() {
    std::printf("#2135 a :subscribers[N] edge leaves with the session that admitted it:\n");
    reset();
    graph_t g;
    fwd_router_t router(g);
    const auto src = g.register_vertex(*path_t::parse("/src"), role_t::STORED_VALUE);
    (void)g.register_vertex(*path_t::parse("/a"), role_t::HANDLER, record_as("a"));
    (void)g.register_vertex(*path_t::parse("/b"), role_t::HANDLER, record_as("b"));
    (void)g.register_vertex(*path_t::parse("/c"), role_t::HANDLER, record_as("c"));
    auto link = tr::mem::make_poly<httpd_ws_link_t>(tr::mem::net_source(), handle(), "/ws",
                                                    tr::net::httpd_ws_config_t{.peer_named = true});
    link->set_auth_cb(&accept_as, nullptr);
    check(router.attach_link("ws", *link), "the link is a routed child");
    // Two edges, both to `/c`, held by `role:operator`: the slots the sessions replace (a
    // replace needs an active slot the caller holds), so a delivery to `/a` or `/b` can only
    // come from a session's edge.
    const std::vector<std::byte> to_c = subscriber_to("c");
    const auto seed = [&] {
        return g.write(src, path_t::parse("/src:subscribers[]")->field(),
                       tr::view::rope_t{*tr::view::over_bytes(to_c)}, "role:operator");
    };
    check(seed().has_value() && seed().has_value(), "role:operator holds slots 0 and 1");

    constexpr int kFirst = 840;
    constexpr int kSecond = 841;
    g_subject = "role:operator";
    for (const auto& [fd, slot, target] : {std::tuple{kFirst, 0u, std::string_view("a")},
                                           std::tuple{kSecond, 1u, std::string_view("b")}}) {
        (void)fake_httpd::instance().open_session(fd);
        (void)fake_httpd::instance().deliver_frame(fd, kCredential);
        (void)fake_httpd::instance().deliver_frame(
            fd, b_fwd(fwd_op_t::WRITE, b_path({"src"}), b_path({"origin"}), subscribers_slot(slot),
                      subscriber_to(target)));
        run_server();
    }
    std::vector<std::string> got = publish_src(g);
    check(reached(got, "a") && reached(got, "b") && !reached(got, "c"),
          "both slots were replaced, and both edges deliver while both sessions are in");

    fake_httpd::instance().close_session(kFirst);
    run_server();
    got = publish_src(g);
    check(!reached(got, "a"), "the first session's edge left with it");
    check(reached(got, "b"), "and the second session's edge, under the same subject, stayed");

    fake_httpd::instance().close_session(kSecond);
    run_server();
    got = publish_src(g);
    check(got.empty(), "the second session's edge left with it too");
}

}  // namespace

int main() {
    std::printf("httpd_ws_link session subject suite (#1846):\n");
    signed_session_is_the_caller(/*peer_named=*/true);
    signed_session_is_the_caller(/*peer_named=*/false);
    no_hook_keeps_the_link_name();
    unfit_subject_is_refused();
    slot_edge_leaves_with_its_session();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
