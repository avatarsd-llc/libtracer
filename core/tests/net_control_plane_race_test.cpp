/**
 * @file
 * @brief ADR-0063 §3 — control-plane writers are serialized, and the forward reader is not.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0014 made connection create/remove a RUNTIME operation, and `make_connection` runs on
 * whichever transport's RECEIVE thread delivered the CREATE — so on an ordinary node with two
 * transports, two creates are genuinely concurrent. Nothing serialized them: `add_child`
 * performs a scan-then-append on the registry (two writers could be handed the SAME empty
 * slot, silently losing a child) and an `emplace_back` on the receiver deque (whose spine two
 * writers corrupt), while `add`'s REBIND path plainly wrote `multi_peer` under a plain read
 * from the forward descent.
 *
 * This test is built to FAIL under `-fsanitize=thread` without those fixes, and it is shaped
 * by what would NOT fail:
 *
 *   - **It churns create → remove → create of the SAME name.** Increment 1's append ordering
 *     is correct (the slot is filled before the `link` and `used` release-stores, and readers
 *     acquire-load both), so a test that only adds NEW names races nothing and passes. The
 *     rebind path — which RFC-0014 create/remove churn takes constantly — is the exposed one.
 *   - **Two writers work on DISJOINT name sets.** Same-name writers would merely contend; the
 *     registry corruption needs two appends of two different new names landing together.
 *   - **A reader runs the whole time**, driving the mount descent over the same table, so a
 *     writer-vs-reader race on the slot fields is live rather than theoretical.
 *
 * It is a race detector's test: with TSan it reports, without TSan it is a smoke test that the
 * churn does not crash or lose children. Both are worth running, so it is not TSan-gated.
 *
 * `transport_vertex_control_plane_churn` below is the second half (#881), one layer up. The
 * registry is not the only control-plane table RFC-0014 made runtime-mutable: the connection
 * map and the module declarations are too, and two PUBLIC readers of them —
 * `transport_vertex_t::set_link_state` and `::module_for` — took no lock while
 * `make_connection` / `remove_connection` / `register_module` mutated them under `ctl_m_`.
 * The deployment shape is what makes it reachable: liveness is reported from a TRANSPORT
 * thread, creation arrives on a RECEIVE thread. That section drives exactly those two
 * threads at each other.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::link_state_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A link that counts sends and never blocks — the churn must not be I/O-bound. */
struct sink_t : tr::net::transport_t {
    std::atomic<std::size_t> sends{0};
    void send(std::span<const std::byte>) override {
        sends.fetch_add(1, std::memory_order_relaxed);
    }
    void send(std::span<const std::span<const std::byte>>) override {
        sends.fetch_add(1, std::memory_order_relaxed);
    }
};

void emit_path(std::vector<std::byte>& out, std::span<const std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

/** @brief `FWD{ op=WRITE, dst, src, VALUE }` — the frame the forward descent walks. */
std::vector<std::byte> make_fwd(std::span<const std::string_view> dst) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    const std::string_view reply[] = {"reply"};
    emit_path(body, std::span<const std::string_view>(reply, 1));
    const std::byte payload[2] = {std::byte{0xAB}, std::byte{0xCD}};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 2));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/**
 * @brief `FWD{ op=WRITE, dst=PATH_REF{2 elements}, src, VALUE }` — a BOUND forward hop.
 *
 * The second reader shape, and it exists because the bound hop reads a DIFFERENT table from
 * the mount descent: it walks the router's per-child receiver contexts to turn an element's
 * slot index into an egress, while `add_child` is appending to that same table from a control
 * thread. Walking the owning deque there is a race on its chunk map — the container-level twin
 * of ADR-0063 erratum 3 — so the contexts are published through an append-only atomic chain
 * and this frame is what drives a reader down it. The route names no slot of this node, so the
 * frame is dropped after the walk; the walk is the point.
 */
std::vector<std::byte> make_bound_fwd() {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    const tr::wire::path_ref_element_t route[2] = {{.index = 3, .generation = 0},
                                                   {.index = 4, .generation = 0}};
    (void)tr::wire::emit_path_ref(body, std::span<const tr::wire::path_ref_element_t>(route, 2));
    const std::string_view reply[] = {"reply"};
    emit_path(body, std::span<const std::string_view>(reply, 1));
    const std::byte payload[2] = {std::byte{0xAB}, std::byte{0xCD}};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 2));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief How many create/remove cycles each writer runs — bounded, so CI cannot hang. */
constexpr int kRounds = 4000;
/** @brief Distinct names per writer. Small, so the churn keeps REBINDING rather than growing. */
constexpr std::size_t kNamesPerWriter = 4;

/** @name The #881 `transport_vertex_t` half */
/**@{*/

/** @brief An owned view over @p bytes — the value shape `graph_t::write` takes. */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief `SPEC{ NAME "type" "client", NAME "name" <name>, SETTINGS "config"{ role=DIAL } }`.
 *
 * The creation frame written to `/net:children[]`, i.e. the PRODUCTION wiring: the graph's
 * child-type catalog is what reaches `make_connection`, so the churn exercises the same entry
 * a peer's CREATE does rather than a private back door (the RFC-0014 lesson).
 */
view_t conn_spec(std::string_view name) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(conn_role_t::DIAL)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));

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

/** @brief Create/remove cycles the connection writer runs — bounded, so CI cannot hang. */
constexpr int kConnRounds = 1500;
/** @brief Connection names churned, all under one module. Small: the point is the rebind. */
constexpr std::size_t kConnNames = 4;
/** @brief The module every churned connection mounts under (`/net/m/<name>`). */
constexpr std::string_view kChurnModule = "m";
/** @brief The `kind` whose module declaration the declarer rewrites under the resolver. */
constexpr std::string_view kChurnKind = "k";

/**
 * @brief #881 — the two public readers of `ctl_m_`-guarded state, driven at their writers.
 *
 * Four threads, two writer/reader pairs over ONE `transport_vertex_t`:
 *
 *   - **`set_link_state` vs create/remove.** The creator stages a link, writes the creating
 *     SPEC and tears the connection down again, so `conns_` takes an `insert_or_assign` and an
 *     `erase` per cycle; the liveness thread calls the PUBLIC `set_link_state` throughout.
 *     Unfixed, that `find` walks the map mid-rebalance and can be handed the very node the
 *     erase is destroying — the returned `conn_t::vertex` is then read after free.
 *   - **`module_for` vs `register_module`.** The declarer rewrites an existing declaration's
 *     module string in place AND periodically appends a new one, so the resolver's walk faces
 *     both a mutating `std::string` and the vector reallocation that invalidates the walk
 *     outright. The string rewrite is the dense signal; the append is the reported one.
 *
 * Both readers are the entry an application actually calls, so this fails at the surface the
 * fix moved. With TSan it reports; without it, it is a smoke test that the churn completes and
 * that both tables end in the state the writers left them.
 */
void transport_vertex_control_plane_churn() {
    std::printf(
        "\ntransport_vertex_t control plane: public liveness/module reads vs their writers "
        "(#881)\n");

    graph_t g;
    fwd_router_t router(g);
    transport_vertex_t net(g, router);

    // Qualified keys are precomputed: the readers must spin on the LOOKUP, not on rebuilding
    // a string, or the window they are meant to hit is buried under allocation.
    std::vector<std::string> names, qualified;
    for (std::size_t i = 0; i < kConnNames; ++i) {
        names.push_back("c" + std::to_string(i));
        qualified.push_back("net/" + std::string(kChurnModule) + "/" + names.back());
    }
    std::array<sink_t, kConnNames> links;

    std::atomic<int> created{0}, removed{0};
    const auto creator = [&] {
        for (int r = 0; r < kConnRounds; ++r) {
            for (std::size_t i = 0; i < kConnNames; ++i) {
                // A staged link is CONSUMED by a successful creation, so it is re-staged every
                // round; `provide_link` is itself a `ctl_m_` mutation.
                net.provide_link(std::string(kChurnModule), names[i], links[i]);
                if (g.write(path_t("/net:children[]"), conn_spec(names[i])))
                    created.fetch_add(1, std::memory_order_relaxed);
                if (net.remove_connection(qualified[i]))
                    removed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    const auto declarer = [&] {
        for (int r = 0; r < kConnRounds; ++r) {
            // Rewrite the SAME (kind, role)'s module in place — two names either side of the
            // small-string boundary, so the assignment touches heap storage the resolver may
            // be copying out.
            (void)net.register_module(
                (r & 1) ? "uplink" : "uplink-long-enough-to-leave-the-small-string-buffer",
                std::string(kChurnKind), conn_role_t::DIAL);
            // And grow the vector now and then: a push_back reallocates it under the walk.
            if (r % 8 == 0)
                (void)net.register_module("g", std::string(kChurnKind) + std::to_string(r),
                                          conn_role_t::DIAL);
        }
    };

    std::atomic<bool> stop{false};
    const auto liveness = [&] {
        while (!stop.load(std::memory_order_relaxed))
            for (const std::string& q : qualified) (void)net.set_link_state(q, link_state_t::UP);
    };
    const auto resolver = [&] {
        while (!stop.load(std::memory_order_relaxed))
            (void)net.module_for(kChurnKind, conn_role_t::DIAL);
    };

    std::thread live(liveness);
    std::thread resolve(resolver);
    std::thread make(creator);
    std::thread declare(declarer);
    make.join();
    declare.join();
    stop.store(true, std::memory_order_relaxed);
    live.join();
    resolve.join();

    // Non-vacuity for the instrument itself: a churn that created nothing races nothing. Every
    // cycle must have both created and removed, or the two threads never overlapped on a
    // populated table.
    const int want = kConnRounds * static_cast<int>(kConnNames);
    check(created.load() == want && removed.load() == want,
          "every create/remove cycle ran — the liveness reader faced a populated table");

    // The writers left both tables in a definite state, and the readers must not have
    // disturbed it: the last act per name was a removal, and the declarer's last write wins.
    int lingering = 0;
    for (const std::string& q : qualified)
        if (net.settings_of(q) != nullptr) ++lingering;
    check(lingering == 0, "no connection outlived its removal (conns_ survived the churn intact)");

    const auto declared = net.module_for(kChurnKind, conn_role_t::DIAL);
    check(declared.has_value() &&
              (*declared == "uplink" || *declared == "uplink-long-enough-to-leave-the-small-string-"
                                                     "buffer"),
          "module_for answers one of the churned declarations, not a torn string");
}

/**@}*/

}  // namespace

int main() {
    std::printf("control-plane create/remove churn against a live forward reader (ADR-0063)\n");

    graph_t g;
    fwd_router_t router(g);

    // The inbound link is registered once and never churned — the reader needs a stable door.
    sink_t inbound;
    router.add_child("net/in/up", inbound);

    // Every sink outlives the router's use of it: `add_child` stores the address, so a sink
    // destroyed while registered would be a use-after-free unrelated to what is under test.
    std::array<sink_t, 2 * kNamesPerWriter> sinks;

    const auto writer = [&](int w) {
        for (int r = 0; r < kRounds; ++r) {
            for (std::size_t i = 0; i < kNamesPerWriter; ++i) {
                std::string name = "net/";
                name += static_cast<char>('a' + w);
                name += "/c";
                name += static_cast<char>('0' + static_cast<char>(i));
                sink_t& s = sinks[static_cast<std::size_t>(w) * kNamesPerWriter + i];
                // create -> remove -> create of the SAME name: the second add takes the
                // REBIND path over the tombstone, which is the exposed one.
                router.add_child(name, s);
                (void)router.remove_child(name);
                router.add_child(name, s);
            }
        }
    };

    std::atomic<bool> stop{false};
    const auto reader = [&] {
        const std::string_view dst[] = {"net", "a", "c0", "leaf"};
        const std::vector<std::byte> frame = make_fwd(std::span<const std::string_view>(dst, 4));
        const std::vector<std::byte> bound = make_bound_fwd();
        while (!stop.load(std::memory_order_relaxed)) {
            // Drives resolve_mount_segs over the churning table: the mount descent reads each
            // slot's name, `multi_peer` and `link` with no lock, by design (ADR-0038 §3).
            router.on_frame("net/in/up", frame);
            // And the BOUND hop's own table walk, against the same churn (RFC-0024 §5.1).
            router.on_frame("net/in/up", bound);
        }
    };

    std::thread ra(reader);
    std::thread rb(reader);
    std::thread wa(writer, 0);
    std::thread wb(writer, 1);
    wa.join();
    wb.join();
    stop.store(true, std::memory_order_relaxed);
    ra.join();
    rb.join();

    // Every name each writer touched must still resolve. A LOST child is exactly what two
    // writers handed the same appended slot produces, and it is otherwise silent.
    int missing = 0;
    for (int w = 0; w < 2; ++w) {
        for (std::size_t i = 0; i < kNamesPerWriter; ++i) {
            std::string name = "net/";
            name += static_cast<char>('a' + w);
            name += "/c";
            name += static_cast<char>('0' + static_cast<char>(i));
            if (router.registry().by_name(name) == nullptr) ++missing;
        }
    }
    check(missing == 0, "every churned name still resolves — no child was lost to a shared slot");

    // And no name grew a SECOND slot. `add` is one-to-one by contract (#524's shadow-slot
    // use-after-free), so the live count is exactly the inbound link plus each writer's names
    // — a duplicate would inflate it even while every lookup above still succeeded.
    check(router.registry().live_size() == 1 + 2 * kNamesPerWriter,
          "and no name grew a second slot — the name→slot mapping stayed one-to-one");

    transport_vertex_control_plane_churn();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
