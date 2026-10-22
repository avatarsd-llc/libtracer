/**
 * @file
 * @brief #2239 — each graph seam is installed on its own and owned by whoever installed it,
 *        and `set_policy` changes only the members it names.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Before #2239 `graph_t::set_hooks` replaced every seam at once, so an application that
 * installed its subject seam after building a router (`g.set_hooks({.subject_lookup = …})`)
 * silently unplugged remote delivery, and `~fwd_router_t` left its three seams pointing at
 * freed memory. `set_policy` likewise applied the whole policy, so a member left at its
 * default reset that property. Each case below pins one of those, with a positive control so
 * a router that had simply stopped delivering could not pass.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::app_field_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_hooks_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::retention_t;
using tr::graph::role_t;
using tr::graph::share_hint_t;
using tr::graph::value_ref_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;
using tr::testing::make_value;

/** @brief A transport that counts the frames the router sends it. */
class count_link_t : public transport_t {
   public:
    /** @brief Count one contiguous frame. */
    void send(std::span<const std::byte> /*frame*/) override {
        const std::lock_guard lock(m_);
        ++sent_;
    }
    /** @brief Count one gathered frame. */
    void send(std::span<const std::span<const std::byte>> /*iov*/) override {
        const std::lock_guard lock(m_);
        ++sent_;
    }
    /** @brief Frames sent since the last call, and reset. */
    std::size_t take() {
        const std::lock_guard lock(m_);
        return std::exchange(sent_, 0);
    }

   private:
    std::mutex m_;         /**< @brief Guards @ref sent_. */
    std::size_t sent_ = 0; /**< @brief Frames sent since the last @ref take. */
};

/** @brief `FIELD{ NAME "subscribers", VALUE u8 ELEMENT }` — the `:subscribers[]` append. */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "subscribers");
    const std::byte mode{1};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&mode, 1));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief `SUBSCRIBER{ PATH /sink }` — the minimal wire subscriber. */
std::vector<std::byte> b_subscriber_sink() {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({"sink"}));
    return out;
}

/** @brief Subscribe the peer on link `peer` to @p g's `/src`, through @p router. */
void subscribe_peer(fwd_router_t& router) {
    router.on_frame("peer", b_fwd(fwd_op_t::WRITE, b_path({"src"}), b_path({"sink"}),
                                  b_field_subscribers_append(), b_subscriber_sink()));
}

/** @brief An application subject seam that names the caller as the subject. */
std::expected<void, tr::wire::err_t> caller_is_subject(void* /*ctx*/, std::string_view caller,
                                                       tr::mem::bytes_t& out) {
    if (!out.append(reinterpret_cast<const std::byte*>(caller.data()), caller.size()))
        return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    return {};
}

/** @brief An application `:stats` sampler that answers nothing. */
bool app_sampler(void* /*ctx*/, std::string_view /*seam_class*/, std::string_view /*seam_name*/,
                 tr::graph::stats_block_t* /*out*/) {
    return false;
}

/**
 * @brief Installing the subject seam AFTER a router leaves the router's remote delivery
 *        plugged in: a write still leaves the node.
 */
void subject_seam_after_router_keeps_remote_delivery() {
    std::printf("installing the subject seam after a router keeps remote delivery (#2239):\n");
    graph_t g;
    fwd_router_t router(g);
    count_link_t link;
    check(router.attach_link("peer", link), "the peer link attaches");
    const vertex_handle_t src = g.register_vertex(path_t("/src"), role_t::STORED_VALUE);
    subscribe_peer(router);
    (void)link.take();  // the subscribe's REPLY

    check(g.write(src, make_value({0x01})).has_value(), "control: the producer writes");
    check(link.take() == 1, "control: the write leaves the node before the subject seam");

    // The application's seam, installed the way #2239 found broken.
    g.set_hook(&graph_hooks_t::subject_lookup, {&caller_is_subject, nullptr});
    check(g.hook(&graph_hooks_t::remote_delivery).ctx == &router,
          "the router's remote-delivery seam is still the router's");
    check(g.write(src, make_value({0x02})).has_value(), "the producer writes again");
    check(link.take() == 1, "the write still leaves the node after the subject seam");
    check(g.hook(&graph_hooks_t::subject_lookup).fn == &caller_is_subject,
          "and the subject seam is the application's");
}

/**
 * @brief A write to a remote-edged vertex after its router is destroyed does not call into
 *        the router (ASan reports a use-after-free if it does), and the router clears only the
 *        seams that are still its own.
 */
void write_after_router_destroyed_calls_nothing() {
    std::printf("a write after the router is destroyed does not call into it (#2239):\n");
    graph_t g;
    count_link_t link;
    const vertex_handle_t src = g.register_vertex(path_t("/src"), role_t::STORED_VALUE);
    auto router = std::make_unique<fwd_router_t>(g);
    check(router->attach_link("peer", link), "the peer link attaches");
    subscribe_peer(*router);
    (void)link.take();
    check(g.write(src, make_value({0x01})).has_value() && link.take() == 1,
          "control: the remote edge delivers while the router lives");

    // The application takes over one seam the router installed; the router must leave it.
    g.set_hook(&graph_hooks_t::stats_sampler, {&app_sampler, &g});
    router.reset();

    check(g.hook(&graph_hooks_t::remote_delivery).fn == nullptr,
          "the router cleared its remote-delivery seam");
    check(g.hook(&graph_hooks_t::wire_target).fn == nullptr,
          "the router cleared its wire-target seam");
    check(g.hook(&graph_hooks_t::stats_sampler).ctx == &g,
          "the router left the sampler the application installed since");
    check(g.write(src, make_value({0x02})).has_value(),
          "a write to the remote-edged vertex still stores");
    check(link.take() == 0, "and sends nothing: no router was called");
}

/** @brief `set_policy` naming one member leaves every other member as it was. */
void partial_set_policy_keeps_the_rest() {
    std::printf("set_policy changes only the members it names (#2239):\n");
    graph_t g;
    const vertex_handle_t s = g.register_vertex(
        path_t("/s"), role_t::STREAM, {},
        {.retention = retention_t::N, .depth = 4, .share_hint = share_hint_t::COPY});
    const auto held = [&g, s]() {
        for (std::uint8_t i = 0; i < 6; ++i) (void)g.write(s, make_value({i}));
        std::vector<value_ref_t> out(8);
        const auto n = g.history(s, out);
        return n ? *n : 0;
    };
    check(held() == 4, "control: the ring holds the declared depth");

    check(g.set_policy(s, {.depth = 2}).has_value(), "set_policy names only the depth");
    check(held() == 2, "the depth moved");
    check(g.retention(s) == retention_t::N, "the retention was kept");
    check(g.share_hint(s) == share_hint_t::COPY, "the share hint was kept");

    check(g.set_policy(s, {.share_hint = share_hint_t::SHARE}).has_value(),
          "set_policy names only the share hint");
    check(g.share_hint(s) == share_hint_t::SHARE, "the share hint moved");
    check(held() == 2, "the depth was kept");

    const vertex_handle_t f =
        g.register_vertex(path_t("/f"), role_t::STORED_VALUE, {},
                          {.app_fields = tr::graph::app_fields_decl_t{app_field_t{
                               .name = "gain", .access = tr::graph::app_access_t::RW}}});
    check(g.write(path_t("/f:settings.app.gain"), make_value({0x07})).has_value() &&
              g.read(path_t("/f:settings.app.gain")).has_value(),
          "control: the declared field takes a value and reads it back");
    check(g.set_policy(f, {.delivery_mode = tr::graph::delivery_mode_t::UNCONDITIONAL}).has_value(),
          "set_policy names only the delivery mode");
    check(g.read(path_t("/f:settings.app.gain")).has_value(),
          "the field table and its value were kept");
    check(g.set_policy(f, {.app_fields = tr::graph::app_fields_decl_t{}}).has_value(),
          "naming an empty table");
    check(!g.read(path_t("/f:settings.app.gain")).has_value(), "uninstalls it");
}

}  // namespace

int main() {
    subject_seam_after_router_keeps_remote_delivery();
    write_after_router_destroyed_calls_nothing();
    partial_set_policy_keeps_the_rest();
    return tr::testing::summary("hook_ownership");
}
