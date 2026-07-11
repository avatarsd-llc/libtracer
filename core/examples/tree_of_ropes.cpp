/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief The three composition axes, made visible — why a libtracer node is a
 *        *tree of ropes*, not a *rope of ropes*.
 *
 * A tempting mental model of libtracer is "one big rope of ropes": a single
 * memory chain that *is* the graph, folds into the TLV tree, and grows every
 * time a transport is attached. That model fuses three things the reference
 * implementation deliberately keeps **orthogonal** (CONTEXT.md §"Two
 * compositions", §"Graph (address) composition") — and that orthogonality is
 * the zero-copy story. This example falsifies the fused model by exercising each
 * axis on its own and asserting they never merge:
 *
 *   1. **Memory composition (L1, `tr::view`)** — a `rope_t` is an ordered chain
 *      of `view_t` windows over refcounted segments (ADR-0053). Its links may
 *      live in *different* backends at once; here link 0 is heap-allocated and
 *      link 1 borrows caller-owned memory, in one chain, with zero byte copies.
 *      A rope is scoped to *one payload* — there is no process-wide rope.
 *
 *   2. **Address composition (L4, `tr::graph`)** — the vertex tree is its own
 *      Composite of `vertex_t` linked by parent/children (ADR-0057). Each leaf
 *      *holds one rope* in its value slot; storing the two-link rope keeps it
 *      two-link (the tree never flattens the memory chain), and a second vertex
 *      holds a wholly separate rope. Tree-of-ropes, not rope-of-ropes.
 *
 *   3. **A transport is an identity, not memory** — mounting a transport
 *      (ADR-0027) via an in-band `/net:children[]` write adds exactly one
 *      addressable `/net/link0` vertex whose value is a tiny link-state rope
 *      (one VALUE TLV), and only once the link reports — a fresh mount holds no
 *      value at all. The transport's real bytes live *outside* the graph, in the
 *      FWD router's
 *      demux; no per-peer vertex or memory is added (ADR-0044). Attaching a bus
 *      does not "grow the rope."
 *
 * The transport half runs over the in-process `loopback_channel_t` (dev/test
 * only) so the example is deterministic and needs no hardware — the same
 * `provide_link` seam accepts a real `transport_can` bus link on a Linux host
 * with a (v)CAN interface, and the structural claims asserted here are
 * identical. This file self-checks and returns non-zero on any mismatch, so it
 * runs as the `example_tree_of_ropes` ctest smoke test. Needs the FWD net plane
 * (`LIBTRACER_NET_PLANE`) for the transport-mount axis.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/loopback.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

/** @brief Print a PASS/FAIL line and tally failures (the smoke-test contract). */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Parse a known-valid path literal (deref is safe for these constants). */
path_t P(std::string_view s) { return *path_t::parse(s); }

/**
 * @brief A connection-creation SPEC, byte-identical to the one in
 *        transport_vertex_test.cpp — the in-band payload a `/net:children[]`
 *        write carries to mount a transport (ADR-0027, reference/05).
 */
view_t conn_spec(std::string_view type, std::string_view name, conn_role_t role,
                 std::uint16_t port) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(role)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::wire::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);

    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body, "config");
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);

    tr::view::segment_ptr_t seg = tr::view::heap_alloc(out.size());
    std::memcpy(seg->bytes.data(), out.data(), out.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief Axis 1 — one rope, two backends, zero byte copies.
 * @return The two-link rope, so the address axis can prove it stores as-is.
 */
rope_t axis1_memory_composition(std::span<std::byte> live) {
    std::printf("Axis 1 (memory, L1): one rope chains links from TWO backends:\n");

    // Link 0 lives in the heap backend.
    tr::view::segment_ptr_t heap_seg = tr::view::heap_alloc(3);
    heap_seg->bytes[0] = std::byte{0xAA};
    heap_seg->bytes[1] = std::byte{0xBB};
    heap_seg->bytes[2] = std::byte{0xCC};

    // Link 1 BORROWS the caller's bytes — no allocation, no copy.
    rope_t r;
    r.append(view_t::over(heap_seg));
    r.append(view_t::over(tr::view::borrow(live)));

    check(r.link_count() == 2, "the rope has two links");
    check(r.total_length() == 3 + live.size(), "logical length spans both segments");
    check(r.links()[0].owner->btag == tr::mem::backend_tag::HEAP, "link 0 is heap-backed");
    check(r.links()[1].owner->btag == tr::mem::backend_tag::BORROWED,
          "link 1 borrows caller memory (a different backend, same chain)");

    // Zero-copy egress: each iovec span points straight into the ORIGINAL segment.
    const std::vector<std::span<const std::byte>> iov = r.to_iovec();
    check(iov.size() == 2, "to_iovec yields one span per link");
    check(iov[1].data() == live.data(),
          "the borrowed link's iovec points INTO the caller's buffer (no copy)");
    return r;
}

/** @brief Axis 3 — the vertex tree holds ropes; it is not one. */
void axis3_address_composition(const rope_t& two_link) {
    std::printf("Axis 3 (address, L4): each vertex HOLDS one rope; the tree is separate:\n");

    graph_t g;
    const auto temp = g.register_vertex(P("/sensor/temp"), role_t::STORED_VALUE);
    const auto humidity = g.register_vertex(P("/sensor/humidity"), role_t::STORED_VALUE);

    const auto w = g.write(temp, two_link);  // rope by value = refcount bumps, never a byte copy
    check(w.has_value(), "write threads the L1 rope into the L4 vertex slot");

    const auto rd = g.read(temp);
    check(rd.has_value() && rd->link_count() == 2,
          "the vertex stored the rope AS-IS — still two links; the tree did not flatten it");
    check(rd.has_value() && rd->links()[1].owner->btag == tr::mem::backend_tag::BORROWED,
          "the borrowed link survived the store (zero copy through the graph)");

    // A second vertex holds a wholly separate rope — there is no global rope.
    std::array<std::byte, 1> hbyte{std::byte{0x42}};
    const auto wh = g.write(humidity, view_t::over(tr::view::borrow(hbyte)));
    const auto rh = g.read(humidity);
    check(wh.has_value() && rh.has_value() && rh->total_length() == 1,
          "humidity holds a DIFFERENT rope — two vertices, two ropes, no shared chain");

    // The address axis is walked by path, independent of any rope's links.
    check(g.find(P("/sensor/temp").key()).has_value() &&
              g.find(P("/sensor/humidity").key()).has_value(),
          "both leaves resolve by walking the vertex Composite (parent/children, not links)");
}

/** @brief The transport-mount axis — an identity vertex, not a memory chain. */
void axis_transport_is_identity() {
    std::printf("Transport mount: adds ONE identity vertex, NOT memory:\n");

    graph_t g;
    fwd_router_t router(g);
    transport_vertex_t net(g, router);

    tr::net::loopback_channel_t channel;  // in-process, deterministic, no hardware
    net.provide_link("link0", channel.a());

    const auto cw =
        g.write(P("/net:children[]"), conn_spec("client", "link0", conn_role_t::DIAL, 8080));
    check(cw.has_value(),
          "mounting a transport is an in-band :children[] write (no new primitive)");

    const auto link_h = g.find(P("/net/link0").key());
    check(link_h.has_value(), "the mount added exactly ONE addressable vertex: /net/link0");

    // A fresh mount is pure identity: an address with NO stored value until the link
    // reports state (a provided link reports via set_link_state; a dialled socket
    // auto-reports on bring-up).
    const auto fresh = g.read(*link_h);
    check(!fresh.has_value() && fresh.error() == status_t::NOT_FOUND,
          "the fresh identity holds NO memory — a mount adds an address, not a value");

    // Once the link reports, the value is a tiny link-state TLV — a SINGLE-link rope,
    // categorically not the sensor's two-link memory chain.
    (void)net.set_link_state("link0", true);
    const auto ls = g.read(*link_h);
    check(ls.has_value() && ls->link_count() == 1 && ls->total_length() <= 8,
          "link-state is a single-link rope of a few bytes — never a chained payload");
    check(router.registry().by_name("link0") == &channel.a(),
          "the transport's real bytes live OUTSIDE the graph, in the router's demux");

    channel.shutdown();  // join recv threads before the router/graph go away
}

}  // namespace

/** @brief Run the three axes; return non-zero on any failed self-check. */
int main() {
    std::printf("tree-of-ropes: three orthogonal compositions, never fused\n\n");

    std::array<std::byte, 4> live{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                  std::byte{0x04}};
    const rope_t two_link = axis1_memory_composition(live);
    std::printf("\n");
    axis3_address_composition(two_link);
    std::printf("\n");
    axis_transport_is_identity();

    std::printf("\n%s: 'rope of ropes' is false — a node is a TREE of ropes.\n",
                g_failures == 0 ? "OK" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
