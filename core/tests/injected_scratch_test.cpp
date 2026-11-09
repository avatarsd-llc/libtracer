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
 * - (c) a folded `:children` read and a composed subtree read build each member's `NAME`
 *       header-and-text from the graph's value backend (#2665): every peak is still zero.
 *       Before #2665 the name text
 *       was borrowed through the one-argument `view::borrow_const`, whose control block came
 *       from `tr::mem::value_source()`.
 * - (d) a `borrow_const` write through a `borrowed_backend_t` built over the graph's value
 *       source (#2660): the borrowed segment's control block is drawn from the injected root,
 *       and every peak is still zero. Before #2660 the only borrow drew it from
 *       `tr::mem::value_source()`. A source that refuses the control block answers a null
 *       segment, so the caller answers BACKPRESSURE and the vertex keeps its value.
 * - (e) the same through the writable `borrow` (#2665): a `borrow` write through the graph's
 *       `borrowed_backend_t` draws its control block from the injected root, every peak is
 *       still zero, and a refusing source answers a null segment. Before #2665 the writable
 *       borrow had no seam and drew from `tr::mem::value_source()`.
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
#include "libtracer/mem_borrowed.hpp"
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
    // Case (d)'s borrow seam, over the source the graph is built on. Declared BEFORE the graph:
    // a value the vertex tree still holds at teardown returns its control block through it.
    tr::mem::borrowed_backend_t borrowed(root, tr::mem::mem_space_t::HOST);

    graph_t g(root);
    fwd_router_t router(g, {.link_src = &heap, .rx = &heap, .flat = &backend, .egress = &backend});
    transport_vertex_t net(g, router, "/net", &backend, &heap);
    // A path the app parses draws from the source it names (#2286): here, the graph's own.
    const auto at = [&g](std::string_view text) { return *path_t::parse(text, g.table_source()); };

    // (a) The path_key_t spill: a 34-byte name record, then a copy of the key.
    std::printf("(a) a long name spills to the graph's source:\n");
    std::size_t before = root.draws;
    (void)g.register_vertex(at("/n/a-name-well-past-sixteen-bytes"), role_t::STORED_VALUE);
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
        check(spec_view && g.write(at("/net/bus/conn"), *spec_view).has_value(),
              "the bus connection is created");
        check(net.set_link_state("net/bus/b0", tr::net::link_state_t::UP).has_value(),
              "its liveness is published");
        report_default_root();
        check(default_root_peak() == 0,
              "creating it and publishing its liveness took nothing from the default root");
        before = root.draws;
        const auto listing = g.read(at("/net/bus/b0:children[]"));
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

    // (c) The folded reads' member names (#2665): the reads draw the graph's source, and the
    // peaks show they cost the default root nothing.
    std::printf("(c) folded reads frame member names from the graph's source:\n");
    const auto fold_draws = [&](std::string_view parent, int children) {
        (void)g.register_vertex(at(parent), role_t::STORED_VALUE);
        for (int i = 0; i < children; ++i) {
            std::string child(parent);
            child += "/c";
            child += static_cast<char>('0' + i);
            (void)g.register_vertex(at(child), role_t::STORED_VALUE);
        }
        std::string field(parent);
        field += ":children";
        const std::size_t first = root.draws;
        const auto listing = g.read(at(field));
        check(listing.has_value() &&
                  (*listing)->link_count() == 1 + static_cast<std::size_t>(children),
              "the :children listing reads back as one link per member, plus the outer one");
        return root.draws - first;
    };
    const std::size_t two = fold_draws("/f2", 2);
    const std::size_t three = fold_draws("/f3", 3);
    std::printf("  :children read: %zu draws with 2 members, %zu with 3\n", two, three);
    check(two > 0 && three > two, "the :children reads drew the graph's source");
    const std::size_t first = root.draws;
    const auto sub = g.read(at("/f3"));  // a branch read is the composed subtree fold
    check(sub.has_value() && (*sub)->link_count() == 4, "the composed read is one link per node");
    std::printf("  composed read of 1 root + 3 named members: %zu draws\n", root.draws - first);
    report_default_root();
    check(default_root_peak() == 0, "and the folded reads took nothing from the default root");

    // (d) A borrowed value written to a vertex: its control block is the graph's draw.
    std::printf("(d) a borrow_const write draws its control block from the graph's source:\n");
    {
        static constexpr std::byte kFour[4] = {std::byte{1}, std::byte{2}, std::byte{3},
                                               std::byte{4}};
        const auto v = g.register_vertex(at("/c/v"), role_t::STORED_VALUE);
        check(&g.value_source() == &root, "the graph's value source is the injected root");
        before = root.draws;
        tr::view::segment_ptr_t seg = tr::view::borrow_const(std::span(kFour), borrowed);
        check(seg && seg->backend == &borrowed, "the borrow is a segment of the graph's backend");
        check(root.draws == before + 1, "its control block is one draw from the graph's source");
        check(g.write(v, tr::view::view_t::over(std::move(seg))).has_value(), "the write lands");
        const auto stored = g.read(v);
        check(stored.has_value() && (*stored)->link_count() == 1 &&
                  (*stored)->only().owner->backend == &borrowed &&
                  (*stored)->only().bytes().data() == kFour,
              "the vertex holds the borrowed bytes in place");
        report_default_root();
        check(default_root_peak() == 0, "and the borrow took nothing from the default root");

        // A refusing source: the borrow answers a null segment, the BACKPRESSURE the caller
        // answers instead of writing, as with `over_bytes`' nullopt.
        tr::mem::borrowed_backend_t refusing(tr::mem::null_source(), tr::mem::mem_space_t::HOST);
        static constexpr std::byte kOther[2] = {std::byte{9}, std::byte{9}};
        check(!tr::view::borrow_const(std::span(kOther), refusing),
              "a refused control block is a null segment");
        const auto kept = g.read(v);
        check(kept.has_value() && (*kept)->link_count() == 1 &&
                  (*kept)->only().bytes().data() == kFour,
              "and the vertex keeps its value");
    }

    // (e) A writable borrow written to a vertex: its control block is the graph's draw too.
    std::printf("(e) a borrow write draws its control block from the graph's source:\n");
    {
        // Static: the vertex still holds these bytes when the graph tears down.
        static std::byte live[3] = {std::byte{5}, std::byte{6}, std::byte{7}};
        const auto v = g.register_vertex(at("/c/w"), role_t::STORED_VALUE);
        before = root.draws;
        tr::view::segment_ptr_t seg = tr::view::borrow(std::span(live), borrowed);
        check(seg && seg->backend == &borrowed, "the borrow is a segment of the graph's backend");
        check(root.draws == before + 1, "its control block is one draw from the graph's source");
        check(g.write(v, tr::view::view_t::over(std::move(seg))).has_value(), "the write lands");
        const auto stored = g.read(v);
        check(stored.has_value() && (*stored)->link_count() == 1 &&
                  (*stored)->only().owner->backend == &borrowed &&
                  (*stored)->only().bytes().data() == live,
              "the vertex holds the borrowed bytes in place");
        report_default_root();
        check(default_root_peak() == 0, "and the borrow took nothing from the default root");

        tr::mem::borrowed_backend_t refusing(tr::mem::null_source(), tr::mem::mem_space_t::HOST);
        std::byte other[2] = {std::byte{9}, std::byte{9}};
        check(!tr::view::borrow(std::span(other), refusing),
              "a refused control block is a null segment");
        const auto kept = g.read(v);
        check(kept.has_value() && (*kept)->link_count() == 1 &&
                  (*kept)->only().bytes().data() == live,
              "and the vertex keeps its value");
    }
    return tr::testing::summary("injected_scratch");
}
