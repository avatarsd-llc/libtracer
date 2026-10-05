/**
 * @file
 * @brief The app-field plane's on-demand read seam (#1878): `handlers_t::on_app_field_read`
 *        answers a declared `:settings.app.<name>` read with the owner's LIVE value.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Before this seam, a field read could only answer the bytes a field write stored, so an
 * owner that keeps the field's state in its own struct (changed at startup, or by another
 * subsystem) had a read return the last client write instead, including one the owner did not
 * take. The vectors, all through the public graph and router API:
 *
 * 1. a field with the seam answers the owner's bytes on a local read, with nothing ever
 *    written;
 * 2. and the same bytes on a `FWD{READ}` that arrived over a link;
 * 3. after a client write the owner's admission filter REFUSES, a read answers the owner's
 *    bytes; without the seam it does not answer the refused bytes either (the field stays
 *    unset);
 * 4. after a client write the owner's apply seam stores but does not adopt, the read still
 *    answers the owner's live value rather than the client's bytes;
 * 5. a DECLINING seam falls through to the stored bytes, or NOT_FOUND when none are stored;
 * 6. the `:settings.app` and `:settings` container reads list the owner's bytes too;
 * 7. a `wo` field has no read surface, so the seam is never asked.
 */

#include <algorithm>
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
using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::result_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::value_ref_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_ctx_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;
using tr::testing::mailbox_t;
using tr::testing::make_value;

constexpr auto kBudget = 5000ms;

/** @brief A one-byte `VALUE` TLV — the shape every field here holds. */
[[nodiscard]] std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte p[1] = {std::byte{v}};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, tr::wire::opt_t{}, p);
    return out;
}

/** @brief The owner's own state for one field, changed by paths that are not field writes. */
struct owner_t {
    std::mutex m;          /**< @brief Guards the fields below (read on a recv thread). */
    std::uint8_t mode = 0; /**< @brief The live value the seam answers. */
    bool decline = false;  /**< @brief Answer `std::nullopt` instead (vector 5). */
    int reads = 0;         /**< @brief How many times the seam was asked. */
    std::string last_name; /**< @brief The key it was last asked for. */

    /** @brief The seam body: the live value as a VALUE TLV, or a decline. */
    std::optional<value_ref_t> read(std::string_view name) {
        const std::lock_guard lock(m);
        ++reads;
        last_name = std::string(name);
        if (decline) return std::nullopt;
        return value_ref_t::copy(b_value_u8(mode));
    }
};

/** @brief The bytes a read of @p p answers, flattened, or empty on any refusal. */
[[nodiscard]] std::vector<std::byte> read_bytes(graph_t& g, const path_t& p) {
    const auto r = g.read(p);
    if (!r) return {};
    const view_t flat = (**r).flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

/** @brief True iff @p haystack contains @p needle as a contiguous run. */
[[nodiscard]] bool contains(const std::vector<std::byte>& haystack,
                            const std::vector<std::byte>& needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
        if (std::equal(needle.begin(), needle.end(), haystack.begin() + i)) return true;
    return false;
}

/** @brief A `FIELD` selector naming `:settings.app.<name>` on the wire. */
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

/** @brief Declare @p names on @p v as remotely `rw` fields, plus a `wo` field `secret`. */
void declare_fields(graph_t& g, vertex_handle_t v, std::initializer_list<const char*> names) {
    std::vector<app_field_t> table;
    for (const char* n : names) table.push_back(app_field_t{.name = n, .access = app_access_t::RW});
    table.push_back(app_field_t{.name = "secret", .access = app_access_t::WO});
    check(g.set_policy(v, {.app_fields = std::move(table)}).has_value(), "declare the fields");
}

}  // namespace

int main() {
    std::printf("App-field read seam (#1878): a read answers the owner's live value\n");

    graph_t g;
    fwd_router_t router(g);
    transport_vertex_t net(g, router);

    // /dev: the owner keeps `mode` in its own struct, refuses remote writes to it through the
    // admission filter, and answers reads from the struct.
    owner_t owner;
    owner.mode = 0x11;
    handlers_t h;
    auto on_read = [&owner](std::string_view name) { return owner.read(name); };
    h.on_app_field_read = tr::graph::thunk(on_read);
    auto on_admit = [](std::string_view name, const view_t& value,
                       const write_ctx_t& ctx) -> result_t<view_t> {
        if (name == "mode" && !ctx.is_local_owner())
            return std::unexpected(status_t::PERMISSION_DENIED);
        return value;
    };
    h.on_app_field_admit = tr::graph::thunk(on_admit);
    const vertex_handle_t dev =
        g.register_vertex(path_t("/dev"), role_t::STORED_VALUE, std::move(h));
    declare_fields(g, dev, {"mode", "level"});

    // /plain: the same refusing filter and NO read seam — the control for vector 3.
    handlers_t ph;
    ph.on_app_field_admit = tr::graph::thunk(on_admit);
    const vertex_handle_t plain =
        g.register_vertex(path_t("/plain"), role_t::STORED_VALUE, std::move(ph));
    declare_fields(g, plain, {"mode"});

    // /cfg: an apply seam that STORES but does not adopt the write (the owner keeps its own
    // state), and a read seam — vector 4.
    owner_t cfg_owner;
    cfg_owner.mode = 0x55;
    handlers_t ch;
    auto cfg_read = [&cfg_owner](std::string_view name) { return cfg_owner.read(name); };
    ch.on_app_field_read = tr::graph::thunk(cfg_read);
    auto cfg_apply = [](std::string_view, const view_t&) {};
    ch.on_app_field_write = tr::graph::thunk(cfg_apply);
    const vertex_handle_t cfg =
        g.register_vertex(path_t("/cfg"), role_t::STORED_VALUE, std::move(ch));
    declare_fields(g, cfg, {"mode"});

    // One peer link, for the wire vectors.
    tr::net::loopback_channel_t ch_peer;  // far side .a() <-> node .b()
    check(net.register_module("peer", "tcp", conn_role_t::DIAL).has_value(),
          "declare module `peer`");
    net.provide_link("peer", "dev", ch_peer.b());
    check(g.write(path_t("/net/peer/conn"), tr::net::conn_spec("dev", 0)).has_value(),
          "connection created through the /net/<module>/conn door");
    mailbox_t inbox;
    ch_peer.a().set_receiver(&into_mailbox, &inbox);
    const std::vector<std::byte> reply_ep = b_path({"reply-ep"});
    const std::vector<std::byte> mode_field = b_app_field("mode");

    // ----- 1. a local read answers the owner's live bytes -----
    std::printf("vector 1 — a local read:\n");
    const path_t dev_mode("/dev:settings.app.mode");
    check(read_bytes(g, dev_mode) == b_value_u8(0x11),
          "the read answers the owner's value with nothing ever written");
    {
        const std::lock_guard lock(owner.m);
        owner.mode = 0x12;  // changed by a path that is not a field write
    }
    check(read_bytes(g, dev_mode) == b_value_u8(0x12), "and follows the owner's state live");
    {
        const std::lock_guard lock(owner.m);
        check(owner.last_name == "mode", "the seam is handed the key below settings.app.");
    }

    // ----- 2. a read over a link answers the same bytes -----
    std::printf("vector 2 — a FWD{READ} over a link:\n");
    ch_peer.a().send(b_fwd(fwd_op_t::READ, b_path({"dev"}), reply_ep, mode_field));
    const std::vector<std::byte> r2 = inbox.wait(kBudget).value_or(std::vector<std::byte>{});
    const auto d2 = tr::wire::decode(r2);
    check(d2 && d2->children.size() >= 5 && d2->children[3].payload.size() == 1 &&
              static_cast<reply_kind_t>(std::to_integer<std::uint8_t>(
                  d2->children[3].payload[0])) == reply_kind_t::RESULT,
          "the wire read is answered RESULT");
    check(d2 && d2->children.size() >= 5 && d2->children[4].type == type_t::VALUE &&
              d2->children[4].payload.size() == 1 && d2->children[4].payload[0] == std::byte{0x12},
          "carrying the owner's live value");

    // ----- 3. a refused client write never reads back -----
    std::printf("vector 3 — after a client write the owner refused:\n");
    ch_peer.a().send(
        b_fwd(fwd_op_t::WRITE, b_path({"dev"}), reply_ep, mode_field, b_value_u8(0x99)));
    check(inbox.wait(kBudget).has_value(), "the refused write is answered");
    check(read_bytes(g, dev_mode) == b_value_u8(0x12),
          "the read answers the owner's value, not the refused bytes");
    ch_peer.a().send(
        b_fwd(fwd_op_t::WRITE, b_path({"plain"}), reply_ep, mode_field, b_value_u8(0x99)));
    check(inbox.wait(kBudget).has_value(), "the control's refused write is answered");
    const auto plain_read = g.read(path_t("/plain:settings.app.mode"));
    check(!plain_read && plain_read.error() == status_t::NOT_FOUND,
          "without a read seam the refused bytes are not stored either: the field stays unset");

    // ----- 4. a write the apply seam did not adopt -----
    std::printf("vector 4 — after a client write the owner stored but did not adopt:\n");
    ch_peer.a().send(
        b_fwd(fwd_op_t::WRITE, b_path({"cfg"}), reply_ep, mode_field, b_value_u8(0x77)));
    check(inbox.wait(kBudget).has_value(), "the write is answered");
    check(read_bytes(g, path_t("/cfg:settings.app.mode")) == b_value_u8(0x55),
          "the read answers the owner's live value, not the client's bytes");

    // ----- 5. a declining seam falls through -----
    std::printf("vector 5 — a declining seam:\n");
    {
        const std::lock_guard lock(owner.m);
        owner.decline = true;
    }
    const path_t dev_level("/dev:settings.app.level");
    const auto unset = g.read(dev_level);
    check(!unset && unset.error() == status_t::NOT_FOUND,
          "declined with nothing stored: NOT_FOUND");
    check(g.write(dev_level, make_value(b_value_u8(0x33))).has_value(), "the owner stores bytes");
    check(read_bytes(g, dev_level) == b_value_u8(0x33), "declined: the stored bytes answer");
    {
        const std::lock_guard lock(owner.m);
        owner.decline = false;
    }

    // ----- 6. the container reads list the owner's bytes -----
    std::printf("vector 6 — the container reads:\n");
    const std::vector<std::byte> app = read_bytes(g, path_t("/dev:settings.app"));
    check(contains(app, b_value_u8(0x12)), "`:settings.app` lists the owner's `mode`");
    check(contains(app, b_value_u8(0x12)) && !contains(app, b_value_u8(0x33)),
          "and the owner's `level`, not the stored one");
    check(contains(read_bytes(g, path_t("/dev:settings")), b_value_u8(0x12)),
          "`:settings` lists it too");

    // ----- 7. a `wo` field has no read surface -----
    std::printf("vector 7 — a write-only field:\n");
    int before = 0;
    {
        const std::lock_guard lock(owner.m);
        before = owner.reads;
    }
    const auto wo = g.read(path_t("/dev:settings.app.secret"));
    check(!wo && wo.error() == status_t::SCHEMA_NOT_FOUND, "a `wo` field answers SCHEMA_NOT_FOUND");
    {
        const std::lock_guard lock(owner.m);
        check(owner.reads == before, "and the seam was never asked");
    }

    ch_peer.shutdown();
    return tr::testing::summary("app_field_read_hook");
}
