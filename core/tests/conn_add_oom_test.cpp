/**
 * @file
 * @brief `make_connection` under a REFUSING allocator: a connection whose link cannot be
 *        wired into the router is ROLLED BACK, not published as a live-looking dead
 *        connection (#930).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section why Why this is its own executable
 *
 * Same instrument as `mount_add_oom_test`, one layer up. The only allocation on the
 * connection-create path that can fail SOFTLY is the registry chunk
 * (`child_registry_t::append` asks for it with `new (std::nothrow) chunk_t()` and reads a
 * null back), and the only way to drive it from a test is to replace the global NOTHROW
 * `operator new` — a whole-program decision, so it lives in its own binary.
 *
 * The replacement is surgical in two ways. It replaces only the UNALIGNED nothrow form, so
 * `mem::heap_source_t::try_alloc` (which calls the ALIGNED nothrow overload, a distinct
 * function that libstdc++ does not route through this one) keeps serving the graph's
 * control-plane blocks. And it leaves the THROWING form alone, so every `std::string` /
 * `std::vector` / `std::make_unique` on the create path — including the vertex the graph
 * registers and the transport the factory builds — allocates normally. What is armed is
 * exactly the one seam `add_child` reports `false` for.
 *
 * @section what What it pins
 *
 * `transport_vertex_t::make_connection` used to call `router_.add_child(qualified, *link)`
 * as a plain statement and throw the `bool` away. By then the identity vertex was
 * registered and the `conns_` entry inserted, so a refused registry chunk left a GHOST
 * connection: `/net/<module>/<name>` resolving as a vertex, published `UP`, holding its
 * constructed socket — and wired into no registry entry, so no `dst` could route to it, no
 * inbound frame could resolve it, and `remove_child` did not know it existed. Peer-drivable
 * on a bounded node: connections created until the slab exhausts mint ghosts.
 *
 * The fix checks the return and rolls the whole creation back — retire the vertex, erase the
 * `conns_` entry (which destroys the socket), publish no liveness — and answers
 * `BACKPRESSURE`.
 *
 * @section ablation What goes RED if the guard is ablated
 *
 * Restore `router_.add_child(qualified, *link);` (ignoring the result) in
 * `transport_vertex_t::make_connection` and `refused_wiring_rolls_back` goes red on every
 * assertion but the last: the create reports SUCCESS, `/net/fake-client/a` resolves, its
 * value reads back as `UP`, `settings_of` finds the entry, and the constructed link is still
 * alive inside it — the ghost, described field by field.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::status_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

/** @brief Record one assertion. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief An owned view over @p bytes (the SPEC is built before the allocator is armed). */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Live instances of @ref fake_link_t — the rollback must leave this at zero. */
int g_links_alive = 0;
/** @brief Total instances ever built, so "alive == 0" cannot pass by never constructing. */
int g_links_built = 0;

/**
 * @brief A transport the factory can build without a socket, a thread, or a port.
 *
 * The census members are the half a `find` check cannot see: `make_connection` moves the
 * constructed link into the `conns_` entry, so a creation that is not rolled back keeps the
 * socket alive inside a connection nothing can route to or remove.
 */
struct fake_link_t : tr::net::transport_t {
    fake_link_t() {
        ++g_links_alive;
        ++g_links_built;
    }
    ~fake_link_t() override { --g_links_alive; }
    fake_link_t(const fake_link_t&) = delete;
    fake_link_t& operator=(const fake_link_t&) = delete;
    void send(std::span<const std::byte>) override {}
};

/**
 * @brief A connection-creation SPEC (reference/05 §0x0E shape).
 *
 * SPEC{ NAME "type" NAME "client", NAME "name" NAME <name>,
 *       NAME "config" SETTINGS{ NAME "role" VALUE u8 [, NAME "kind" NAME <kind>] } }
 *
 * An empty @p kind omits the key, which is the staged-link (`provide_link`) spelling: the
 * module then follows from the staging rather than from a factory selector.
 */
view_t conn_spec(std::string_view name, std::string_view kind = "fake") {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(conn_role_t::DIAL)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    if (!kind.empty()) {
        tr::wire::emit_name(cfg, "kind");
        tr::wire::emit_name(cfg, kind);
    }

    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, "client");
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body, "config");
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

/**
 * @brief The published 1-byte link-liveness VALUE at @p path, or `0xFF` when the address
 *        holds nothing at all (retired, never registered, or never written).
 */
std::uint8_t link_state_byte(graph_t& g, std::string_view path) {
    const auto h = g.find(path_t::parse(path)->key());
    if (!h) return 0xFF;
    const auto v = g.read(*h);
    if (!v) return 0xFF;
    const auto bytes = (*v)->materialize().bytes();
    // VALUE TLV of a 1-byte payload — the payload is the last byte.
    return bytes.empty() ? 0xFF : static_cast<std::uint8_t>(bytes.back());
}

/** @brief Wire the `fake` kind and its module declaration onto @p net (ADR-0073 §4). */
void declare_fake_module(transport_vertex_t& net) {
    (void)net.register_module("fake-client", "fake", conn_role_t::DIAL);
    net.register_transport_type(
        "fake",
        [](const tr::net::conn_settings_t&,
           const tr::wire::tlv_t*) -> tr::graph::result_t<std::unique_ptr<tr::net::transport_t>> {
            return std::unique_ptr<tr::net::transport_t>(new fake_link_t());
        });
}

}  // namespace

/**
 * @brief True while the global nothrow `operator new` must refuse.
 *
 * Namespace-scope and non-static because the replacement below is a global function and this
 * is the only state it may read. Armed for the width of ONE connection create.
 */
bool g_refuse_nothrow_new = false;

/**
 * @brief The refusing seam: a null back, exactly as an exhausted heap gives.
 *
 * Disarmed, it forwards to the THROWING global form rather than to `std::malloc`, so every
 * block handed out here is still freed by the matching `::operator delete`.
 */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_refuse_nothrow_new) return nullptr;
    try {
        return ::operator new(n);
    } catch (...) {
        return nullptr;
    }
}

namespace {

/** @brief A refused registry chunk means: no vertex, no entry, no socket, no UP — and an error. */
void test_refused_wiring_rolls_back() {
    std::printf("make_connection under a refusing allocator:\n");
    g_links_built = 0;
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_module(net);

    const view_t spec = conn_spec("a");  // built BEFORE the arming
    g_refuse_nothrow_new = true;         // the FIRST chunk the registry asks for is refused
    const auto w = node.write(path_t("/net:children[]"), spec);
    g_refuse_nothrow_new = false;

    check(!w.has_value(), "the create REPORTS the failure rather than returning success");
    check(!w.has_value() && w.error() == status_t::BACKPRESSURE,
          "and the status is BACKPRESSURE (a wiring refusal, not a malformed spec)");
    check(!node.find(path_t::parse("/net/fake-client/a")->key()).has_value(),
          "no identity vertex is left at /net/fake-client/a");
    // Read THROUGH the address rather than asking for the published byte: with the guard
    // ablated the liveness write itself lands under the armed allocator and can publish an
    // empty value, so a "the byte is not UP" check would pass on the ghost. The address
    // answering `tr::path::not_found` is the claim that discriminates.
    check(!node.read(path_t("/net/fake-client/a")).has_value(),
          "reading the address answers not-found — nothing was published there");
    check(net.settings_of("net/fake-client/a") == nullptr, "no conns_ entry survives");
    check(net.link_of("net/fake-client/a") == nullptr, "and no link is held under the name");
    check(router.registry().live_size() == 0, "the router registry holds nothing");
    check(router.registry().by_name("net/fake-client/a") == nullptr,
          "and no dst can resolve the name");
    check(g_links_built == 1 && g_links_alive == 0,
          "the constructed transport was BUILT and then torn down by the rollback");

    // The rollback is total, not partial: the address and the qualified name are both free
    // again, so the very next create under an open allocator takes them.
    const view_t again = conn_spec("a");
    const auto w2 = node.write(path_t("/net:children[]"), again);
    check(w2.has_value(), "the same name creates cleanly afterwards — no half-built residue");
}

/** @brief The same create with the allocator open. The refusal is the ONLY difference, so the
 *         red above cannot be the test refusing everything. */
void test_open_allocator_creates() {
    std::printf("make_connection with the allocator open:\n");
    g_links_built = 0;
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_module(net);

    const view_t spec = conn_spec("a");
    const auto w = node.write(path_t("/net:children[]"), spec);
    check(w.has_value(), "the create succeeds");
    check(node.find(path_t::parse("/net/fake-client/a")->key()).has_value(),
          "the identity vertex resolves");
    check(net.settings_of("net/fake-client/a") != nullptr, "the conns_ entry exists");
    check(router.registry().live_size() == 1, "the child IS registered");
    check(router.registry().by_name("net/fake-client/a") != nullptr,
          "and a dst resolves the name — so the checks above are not vacuous");

    // A constructed DIAL link publishes its liveness: link_state_t::UP is the 1-byte VALUE.
    check(link_state_byte(node, "/net/fake-client/a") ==
              static_cast<std::uint8_t>(tr::net::link_state_t::UP),
          "and UP liveness IS published on the success path");
    check(g_links_built == 1 && g_links_alive == 1, "the constructed transport is held live");
}

/**
 * @brief A STAGED link survives a refused create — the rollback gives back what it took.
 *
 * `provide_link` staging is consumed by the create that uses it, and consuming it before the
 * wiring is known to have succeeded would make the rollback lossy: the connection would be
 * gone AND its link no longer stageable, so a retry once the pressure clears could only
 * answer NOT_FOUND. This is the half of "total" that the vertex/entry checks cannot see.
 */
void test_refused_wiring_leaves_staged_link_reusable() {
    std::printf("make_connection under a refusing allocator, with a STAGED link:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_module(net);
    fake_link_t staged;
    net.provide_link("fake-client", "s", staged);

    const view_t spec = conn_spec("s", {});  // no `kind`: the staged link is the one to use
    g_refuse_nothrow_new = true;
    const auto w = node.write(path_t("/net:children[]"), spec);
    g_refuse_nothrow_new = false;
    check(!w.has_value() && w.error() == status_t::BACKPRESSURE, "the create answers BACKPRESSURE");
    check(!node.find(path_t::parse("/net/fake-client/s")->key()).has_value(),
          "no identity vertex is left behind");

    const view_t again = conn_spec("s", {});
    const auto w2 = node.write(path_t("/net:children[]"), again);
    check(w2.has_value(), "the staged link was NOT consumed — the retry creates the connection");
    check(router.registry().by_name("net/fake-client/s") == &staged,
          "and it is the SAME staged link that ends up wired");
}

}  // namespace

int main() {
    test_refused_wiring_rolls_back();
    test_open_allocator_creates();
    test_refused_wiring_leaves_staged_link_reusable();
    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
