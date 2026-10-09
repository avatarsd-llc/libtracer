/**
 * @file
 * @brief #2052 — with every source injected, a long vertex name, a bus connection's liveness
 *        value and its synthesized `:children[]` listing draw nothing from the default root.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The default root is the static arena on an MCU build and the host slab pool here; the
 * `tr::mem::value_source()`, `table_source()` and `net_source()` accessors are its three
 * sub-pools, and `tr::mem::heap_backend()` draws from the value one. An application that
 * injects its own source into the graph, the router and the transport vertex sizes the arena
 * against what is left, so the instrument is each sub-pool's PEAK, read after each step of a
 * process that injected every source.
 *
 * - (a) a name record past `path_key_t::kInlineBytes`, registered and then copied, spills to
 *       the graph's injected source only (#1991): every peak is still zero.
 * - (b) creating a bus connection and publishing its liveness leave every peak at zero; before
 *       #2052 the liveness value came from `tr::mem::heap_backend()`. A read of the connection
 *       vertex's `:children[]` then stages its listing on, and answers it from, the owning
 *       graph's sources; before #2052 the staging came from `tr::mem::net_source()` and the
 *       answer from `tr::mem::heap_backend()`, and the read's composed wrapper
 *       (`value_ref_t::composed`, one block on every composed field read) from
 *       `tr::mem::value_source()`. Every peak is still zero after the read, the listing's
 *       segment came from the graph's backend and its wrapper from the graph's value source:
 *       the arena minimum of a fully-injected node is 0 B.
 *
 * Every case also checks that its draw reached the injected root (a count of the blocks it
 * served), so a zero on the default root is not the zero of a path that never ran.
 */

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "libtracer/conn_spec.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source_backend.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::testing::check;

/** @brief A point-to-point transport that swallows whatever is sent to it. */
struct sink_link_t : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
};

/** @brief A bus link with a fixed two-peer census, so the listing has members to stage. */
struct fake_bus_t : tr::net::transport_t, tr::net::bus_link_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
    tr::net::bus_link_t* bus() override { return this; }
    tr::net::transport_t* peer_link(std::string_view) override { return &peer; }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t& visit) const override {
        visit("n7");
        visit("n12");
    }
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t p,
                                             std::span<char>) const override {
        return p.valid() ? std::string_view("n7") : std::string_view();
    }
    sink_link_t peer; /**< @brief The directed endpoint every peer name resolves to. */
};

/** @brief The graph's injected root: the heap, with a count of the draws it served. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("test_root") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        ++draws;
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::size_t draws = 0; /**< @brief Blocks served so far. */
};

/** @brief The three default sub-pools' peaks, summed: zero means the root was never drawn. */
std::size_t default_root_peak() {
    return tr::mem::value_source().stats().peak + tr::mem::table_source().stats().peak +
           tr::mem::net_source().stats().peak;
}

/** @brief Print each default sub-pool's peak, so a failure names the one that was drawn. */
void report_default_root() {
    std::printf("  default root: %zu B carved; sub-pool peaks: values %zu, tables %zu, net %zu\n",
                tr::mem::default_root().stats().in_use, tr::mem::value_source().stats().peak,
                tr::mem::table_source().stats().peak, tr::mem::net_source().stats().peak);
}

}  // namespace

int main() {
    std::printf("injected_scratch_test (#2052):\n");
    tr::mem::block_source_t& heap = tr::mem::heap_source();
    tr::mem::source_backend_t backend(heap);
    counting_source_t root;

    graph_t g(root);
    fwd_router_t router(g, {.label_src = &heap, .rx = &heap, .flat = &backend, .egress = &backend});
    transport_vertex_t net(g, router, "/net", &backend, &heap);

    // (a) The path_key_t spill: a 34-byte name record, then a copy of the key.
    std::printf("(a) a long name spills to the graph's source:\n");
    std::size_t before = root.draws;
    (void)g.register_vertex(path_t("/n/a-name-well-past-sixteen-bytes"), role_t::STORED_VALUE);
    check(root.draws > before, "the spill drew the graph's source");
    // The copy constructor draws from the source that served the original (#1991).
    const std::string_view text = "a-record-well-past-sixteen-bytes";
    const auto key =
        tr::graph::path_key_t::try_make(std::as_bytes(std::span(text)), g.table_source());
    check(key.has_value() && key->bytes().size() > tr::graph::path_key_t::kInlineBytes,
          "a key past the inline bytes spills");
    const tr::graph::path_key_t copy = *key;
    check(copy == *key, "and its copy holds the same bytes");
    report_default_root();
    check(default_root_peak() == 0, "and nothing from the default root");

    // (b) A bus connection vertex's `:children[]` listing.
    if constexpr (tr::net::kBusLinks) {
        std::printf("(b) a bus listing stages on and answers from the graph's sources:\n");
        (void)net.register_module("bus", "can", conn_role_t::DIAL);
        fake_bus_t bus;
        net.provide_link("bus", "b0", bus);
        tr::mem::bytes_t spec(heap);
        check(tr::net::conn_spec_t("b0").bytes(spec), "the SPEC encodes");
        const auto spec_view = tr::view::over_bytes(tr::mem::as_span(spec), backend);
        check(spec_view && g.write(path_t("/net/bus/conn"), *spec_view).has_value(),
              "the bus connection is created");
        check(net.set_link_state("net/bus/b0", tr::net::link_state_t::UP).has_value(),
              "its liveness is published");
        report_default_root();
        check(default_root_peak() == 0,
              "creating it and publishing its liveness took nothing from the default root");
        before = root.draws;
        const auto listing = g.read(path_t("/net/bus/b0:children[]"));
        check(
            listing.has_value() && (*listing)->link_count() == 1 && (*listing)->total_length() > 0,
            "the listing reads back as one non-empty link");
        check(root.draws > before, "the read staged on the graph's source");
        check(listing.has_value() && (*listing)->link_count() == 1 &&
                  (*listing)->only().owner->backend == &g.value_backend(),
              "the listing's bytes are a segment from the graph's value backend");
        check(listing.has_value() && (*listing)->source() == &g.value_source(),
              "the composed read's wrapper block is from the graph's value source");
        report_default_root();
        check(default_root_peak() == 0, "and the read took nothing from the default root");
    } else {
        std::printf("(b) skipped: this build closed the bus module (kBusLinks = false)\n");
    }
    return tr::testing::summary("injected_scratch");
}
