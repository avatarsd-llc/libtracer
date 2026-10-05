/**
 * @file
 * @brief The admission context's LINK claim (#1650): `write_ctx_t::link` carries the
 *        transport-catalog `(kind, role)` of the link a write arrived on.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Before this, an admission filter decided on the value and the writer's SUBJECT only, so a
 * policy that admits a write from a dialled peer link and refuses the same write from a session
 * a `ws` listener accepted had nothing to decide on but a link's name. One node here carries two
 * links, each bound through the production `/net/<module>/conn` door under a module declared for
 * a different `(kind, role)`, and one filtered vertex:
 *
 * 1. the SAME `FWD{WRITE}` over the `tcp`/`DIAL` link is admitted, and the filter saw that pair;
 * 2. over the `ws`/`LISTEN` link it is refused, the refusal reaches the writer as an ERROR
 *    reply, and the prior last-known-value stands;
 * 3. the owner's own API write presents a null link;
 * 4. a HANDLER's `on_write` sees the same pair through the same context;
 * 5. the app-field admission filter (`on_app_field_admit`, #1832) receives the same context on
 *    a `:settings.app.<name>` write: the subject and the pair over a link, the empty owner
 *    subject and a null link on the owner's own field write.
 *
 * Vectors 1, 2 and 4 fail with the production hunks reverted: the filter sees a null link on
 * both, so it can tell the two sessions apart only by name. Vector 5 fails to compile with
 * #1832 reverted: the field filter was handed no context at all.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using namespace std::chrono_literals;
using tr::graph::admission_t;
using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_ctx_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::link_kind_t;
using tr::net::transport_vertex_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;
using tr::testing::mailbox_t;

constexpr auto kBudget = 5000ms;

/** @brief What a seam last saw of the arrival link, copied out of the borrowed context. */
struct seen_t {
    std::mutex m;          /**< @brief Guards the fields below (written on a recv thread). */
    int calls = 0;         /**< @brief How many writes reached the seam. */
    bool had_link = false; /**< @brief Whether the last write carried a link record. */
    std::string kind;      /**< @brief Its catalog `kind`, copied out. */
    conn_role_t role{};    /**< @brief Its role. */
    std::string subject;   /**< @brief The writer's subject, copied out. */

    /** @brief Record @p ctx — the copy the borrowed-context contract asks for. */
    void note(const write_ctx_t& ctx) {
        const std::lock_guard lock(m);
        ++calls;
        had_link = ctx.link != nullptr;
        kind = had_link ? std::string(ctx.link->kind) : std::string{};
        role = had_link ? ctx.link->role : conn_role_t{};
        subject = std::string(ctx.subject);
    }
};

/** @brief The bytes stored at @p p — the written TLV a wire write retains — or empty. */
[[nodiscard]] std::vector<std::byte> stored_bytes(graph_t& g, const path_t& p) {
    const auto r = g.read(p);
    if (!r) return {};
    const view_t flat = (**r).flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

/** @brief A one-byte `VALUE` TLV — the written payload. */
[[nodiscard]] std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte p[1] = {std::byte{v}};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, tr::wire::opt_t{}, p);
    return out;
}

/** @brief The creator endpoint of @p module — the one wire door that creates a connection. */
[[nodiscard]] path_t conn_endpoint(std::string_view module) {
    std::string text = "/net/";
    text += module;
    text += "/conn";
    return path_t(text);
}

/** @brief The reply `kind` of a FWD{REPLY}, or nullopt when @p frame is not one. */
[[nodiscard]] std::optional<reply_kind_t> reply_kind(const std::vector<std::byte>& frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD || dec->children.size() < 4) return std::nullopt;
    const auto& k = dec->children[3].payload;
    if (k.size() != 1) return std::nullopt;
    return static_cast<reply_kind_t>(std::to_integer<std::uint8_t>(k[0]));
}

/** @brief A `FIELD` selector naming `:settings.app.<name>` — the wire spelling of the
 *         app-field write door. */
[[nodiscard]] std::vector<std::byte> b_app_field(std::string_view name) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "settings");
    tr::wire::emit_name(body, "app");
    tr::wire::emit_name(body, name);
    std::vector<std::byte> field;
    tr::wire::emit_tlv(field, type_t::FIELD, tr::wire::opt_t{.pl = true}, body);
    return field;
}

/** @brief Hand every frame arriving at a raw far-side endpoint to a @ref mailbox_t. */
void into_mailbox(void* ctx, std::span<const std::byte> frame) {
    static_cast<mailbox_t*>(ctx)->push(std::vector<std::byte>(frame.begin(), frame.end()));
}

}  // namespace

int main() {
    std::printf("Admission link kind (#1650): the filter sees the arrival link's (kind, role)\n");

    graph_t g;
    fwd_router_t router(g);
    transport_vertex_t net(g, router);

    // The filtered vertex: a peer link may write it, a session a `ws` listener accepted may
    // not. The policy is the filter's; the graph only hands it the pair.
    seen_t admit_seen;
    handlers_t h;
    auto on_admit = [&admit_seen](const tr::graph::value_t&,
                                  const write_ctx_t& ctx) -> admission_t {
        admit_seen.note(ctx);
        if (ctx.link != nullptr && ctx.link->is("ws", conn_role_t::LISTEN))
            return std::unexpected(status_t::PERMISSION_DENIED);
        return std::nullopt;
    };
    h.on_admit = tr::graph::thunk(on_admit);
    const vertex_handle_t cfg =
        g.register_vertex(path_t("/cfg"), role_t::STORED_VALUE, std::move(h));

    // A HANDLER beside it, for vector 4.
    seen_t write_seen;
    handlers_t hh;
    auto on_write = [&write_seen](const tr::graph::value_t&,
                                  const write_ctx_t& ctx) -> tr::graph::result_t<void> {
        write_seen.note(ctx);
        return {};
    };
    hh.on_write = tr::graph::thunk(on_write);
    (void)g.register_vertex(path_t("/cmd"), role_t::HANDLER, std::move(hh));

    // An app-field owner, for vector 5: the same policy, spelled on the field plane's filter.
    seen_t field_seen;
    handlers_t fh;
    auto on_field_admit = [&field_seen](std::string_view, const view_t& value,
                                        const write_ctx_t& ctx) -> tr::graph::result_t<view_t> {
        field_seen.note(ctx);
        if (ctx.link != nullptr && ctx.link->is("ws", conn_role_t::LISTEN))
            return std::unexpected(status_t::PERMISSION_DENIED);
        return value;
    };
    fh.on_app_field_admit = tr::graph::thunk(on_field_admit);
    const vertex_handle_t dev =
        g.register_vertex(path_t("/dev"), role_t::STORED_VALUE, std::move(fh));
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "mode", .access = app_access_t::RW});
    check(g.set_policy(dev, {.app_fields = std::move(table)}).has_value(),
          "declare /dev's remotely writable app field `mode`");

    // Two links, two modules, two catalog pairs. Nothing constructs from these kinds — a staged
    // link outranks a factory — but the module's declared pair is what the router interns.
    tr::net::loopback_channel_t ch_peer;     // far side .a() <-> node .b()
    tr::net::loopback_channel_t ch_browser;  // far side .a() <-> node .b()
    check(net.register_module("peer", "tcp", conn_role_t::DIAL).has_value(),
          "declare module `peer` as (tcp, DIAL)");
    check(net.register_module("ws-server", "ws", conn_role_t::LISTEN).has_value(),
          "declare module `ws-server` as (ws, LISTEN)");
    net.provide_link("peer", "dev", ch_peer.b());
    net.provide_link("ws-server", "browser", ch_browser.b());
    check(g.write(conn_endpoint("peer"), tr::net::conn_spec("dev", 0)).has_value() &&
              g.write(conn_endpoint("ws-server"), tr::net::conn_spec("browser", 0)).has_value(),
          "both connections created through the /net/<module>/conn door (production wiring)");

    mailbox_t peer_inbox;
    mailbox_t browser_inbox;
    ch_peer.a().set_receiver(&into_mailbox, &peer_inbox);
    ch_browser.a().set_receiver(&into_mailbox, &browser_inbox);

    const std::vector<std::byte> cfg_dst = b_path({"cfg"});
    const std::vector<std::byte> reply_ep = b_path({"reply-ep"});

    // ----- 1. the peer link's write is admitted, and the filter saw (tcp, DIAL) -----
    std::printf("vector 1 — a write over a dialled peer link:\n");
    const std::vector<std::byte> v22 = b_value_u8(0x22);
    ch_peer.a().send(b_fwd(fwd_op_t::WRITE, cfg_dst, reply_ep, {}, v22));
    const auto r1 = peer_inbox.wait(kBudget);
    check(r1.has_value() && reply_kind(*r1) == reply_kind_t::RESULT,
          "the peer's write is answered RESULT");
    check(stored_bytes(g, path_t("/cfg")) == v22, "and its value is retained");
    {
        const std::lock_guard lock(admit_seen.m);
        check(
            admit_seen.had_link && admit_seen.kind == "tcp" && admit_seen.role == conn_role_t::DIAL,
            "the filter saw the link as (tcp, DIAL)");
        check(!admit_seen.subject.empty(), "beside a non-owner subject");
    }

    // ----- 2. the same write over the ws listener's session is refused -----
    std::printf("vector 2 — the same write over a session a ws listener accepted:\n");
    const std::vector<std::byte> v33 = b_value_u8(0x33);
    ch_browser.a().send(b_fwd(fwd_op_t::WRITE, cfg_dst, reply_ep, {}, v33));
    const auto r2 = browser_inbox.wait(kBudget);
    check(r2.has_value() && reply_kind(*r2) == reply_kind_t::ERROR,
          "the browser session's write is answered ERROR");
    check(stored_bytes(g, path_t("/cfg")) == v22, "and the prior value stands");
    {
        const std::lock_guard lock(admit_seen.m);
        check(admit_seen.calls == 2, "the filter ran once per write");
        check(admit_seen.had_link && admit_seen.kind == "ws" &&
                  admit_seen.role == conn_role_t::LISTEN,
              "the filter saw the link as (ws, LISTEN)");
    }

    // ----- 3. the owner's API write carries no link -----
    std::printf("vector 3 — the owner's own write:\n");
    check(g.write(cfg, rope_t{tr::testing::make_value({0x44})}).has_value(),
          "the owner's write is admitted");
    {
        const std::lock_guard lock(admit_seen.m);
        check(!admit_seen.had_link && admit_seen.subject.empty(),
              "it presents a null link and the empty owner subject");
    }

    // ----- 4. a HANDLER's on_write sees the same pair -----
    std::printf("vector 4 — a HANDLER's on_write:\n");
    ch_browser.a().send(b_fwd(fwd_op_t::WRITE, b_path({"cmd"}), reply_ep, {}, v33));
    const auto r4 = browser_inbox.wait(kBudget);
    check(r4.has_value() && reply_kind(*r4) == reply_kind_t::RESULT, "the handler write answers");
    {
        const std::lock_guard lock(write_seen.m);
        check(write_seen.calls == 1 && write_seen.had_link && write_seen.kind == "ws" &&
                  write_seen.role == conn_role_t::LISTEN,
              "on_write saw the link as (ws, LISTEN)");
    }

    // ----- 5. the app-field filter sees the same context -----
    std::printf("vector 5 — the app-field admission filter:\n");
    const std::vector<std::byte> mode_field = b_app_field("mode");
    const path_t mode_path("/dev:settings.app.mode");
    ch_peer.a().send(b_fwd(fwd_op_t::WRITE, b_path({"dev"}), reply_ep, mode_field, v22));
    const auto r5 = peer_inbox.wait(kBudget);
    check(r5.has_value() && reply_kind(*r5) == reply_kind_t::RESULT,
          "the peer's field write is answered RESULT");
    check(stored_bytes(g, mode_path) == v22, "and its bytes are stored");
    {
        const std::lock_guard lock(field_seen.m);
        check(field_seen.calls == 1 && field_seen.had_link && field_seen.kind == "tcp" &&
                  field_seen.role == conn_role_t::DIAL,
              "the field filter saw the link as (tcp, DIAL)");
        check(!field_seen.subject.empty(), "beside a non-owner subject");
    }
    ch_browser.a().send(b_fwd(fwd_op_t::WRITE, b_path({"dev"}), reply_ep, mode_field, v33));
    const auto r6 = browser_inbox.wait(kBudget);
    check(r6.has_value() && reply_kind(*r6) == reply_kind_t::ERROR,
          "the browser session's field write is answered ERROR");
    check(stored_bytes(g, mode_path) == v22, "and the field's prior bytes stand");
    {
        const std::lock_guard lock(field_seen.m);
        check(field_seen.calls == 2 && field_seen.had_link && field_seen.kind == "ws" &&
                  field_seen.role == conn_role_t::LISTEN,
              "the field filter saw the link as (ws, LISTEN)");
    }
    const std::vector<std::byte> v44 = b_value_u8(0x44);
    check(g.write(mode_path, tr::testing::make_value(v44)).has_value(),
          "the owner's own field write is admitted");
    {
        const std::lock_guard lock(field_seen.m);
        check(field_seen.calls == 3 && !field_seen.had_link && field_seen.subject.empty(),
              "it presents a null link and the empty owner subject");
    }

    // The predicate a filter spells its policy with matches both halves of the pair.
    check(link_kind_t{.kind = "ws", .role = conn_role_t::LISTEN}.is("ws", conn_role_t::LISTEN) &&
              !link_kind_t{.kind = "ws", .role = conn_role_t::DIAL}.is("ws", conn_role_t::LISTEN),
          "link_kind_t::is matches kind AND role");

    ch_browser.shutdown();
    ch_peer.shutdown();
    return tr::testing::summary("admission_link_kind");
}
