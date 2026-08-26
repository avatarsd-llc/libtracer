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

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"
#include "test_support.hpp"

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

using tr::testing::check;

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
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
}

/** @brief `FWD{ op=WRITE, dst, src, VALUE }` — the frame the forward descent walks. */
std::vector<std::byte> make_fwd(std::span<const std::string_view> dst) {
    std::vector<std::byte> payload;
    const std::byte pv[2] = {std::byte{0xAB}, std::byte{0xCD}};
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, std::span<const std::byte>(pv, 2));
    return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, tr::testing::b_path(dst),
                              tr::testing::b_path({"reply"}), {}, payload);
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
    std::vector<std::byte> dst;
    const tr::wire::path_ref_element_t route[2] = {{.index = 3, .generation = 0},
                                                   {.index = 4, .generation = 0}};
    (void)tr::wire::emit_path_ref(dst, std::span<const tr::wire::path_ref_element_t>(route, 2));
    std::vector<std::byte> payload;
    const std::byte pv[2] = {std::byte{0xAB}, std::byte{0xCD}};
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, std::span<const std::byte>(pv, 2));
    return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, dst, tr::testing::b_path({"reply"}), {},
                              payload);
}

/** @brief How many create/remove cycles each writer runs — bounded, so CI cannot hang. */
constexpr int kRounds = 4000;
/** @brief Distinct names per writer. Small, so the churn keeps REBINDING rather than growing. */
constexpr std::size_t kNamesPerWriter = 4;

/** @name The #881 `transport_vertex_t` half */
/**@{*/

/**
 * @brief `SPEC{ NAME "name" NAME <name> }` — the config-less, staged-link creation payload.
 *
 * The creation frame written to `/net/<module>/conn`, i.e. the PRODUCTION wiring: the module's
 * creator endpoint is what reaches `make_connection`, so the churn exercises the same door a
 * peer's CREATE does rather than a private back door (the RFC-0014 lesson).
 */
view_t conn_spec(std::string_view name) { return tr::net::conn_spec_t(name).view(); }

/** @brief Create/remove cycles the connection writer runs — bounded, so CI cannot hang. */
constexpr int kConnRounds = 1500;
/** @brief Connection names churned, all under one module. Small: the point is the rebind. */
constexpr std::size_t kConnNames = 4;
/** @brief The module every churned connection mounts under (`/net/m/<name>`). */
constexpr std::string_view kChurnModule = "m";
/** @brief The `kind` the resolver thread resolves while the declarer appends under it. */
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
 *   - **`module_for` vs `register_module`.** The declarer appends a fresh declaration every
 *     round — alternating two module names either side of the small-string boundary — so the
 *     resolver's walk faces the vector reallocation that invalidates it outright, which is
 *     the reported signal. It no longer faces a `std::string` mutating UNDER the walk,
 *     because that hazard was retired at the source: re-declaring an already-declared
 *     *(kind, role)* under a different module is now `PATH_IN_USE` rather than an in-place
 *     rename, so nothing rewrites a recorded module name any more. The declarer is kept
 *     driving the append arm at full rate in its place.
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

    // The churned connections' module, declared once up front: `register_module` mints the
    // `/net/m/conn` creator endpoint, which is the only door creation has. Its kind is empty
    // because every connection here is a STAGED link — nothing is constructed from a factory —
    // and an empty kind also keeps this declaration clear of the (kind, role) slot the declarer
    // below rewrites thousands of times.
    check(net.register_module(std::string(kChurnModule), "", conn_role_t::DIAL).has_value(),
          "the churned module is declared — its creator endpoint is the creation door");
    const path_t conn_endpoint("/net/" + std::string(kChurnModule) + "/conn");

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
                if (g.write(conn_endpoint, conn_spec(names[i])))
                    created.fetch_add(1, std::memory_order_relaxed);
                if (net.remove_connection(qualified[i]))
                    removed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    const auto declarer = [&] {
        for (int r = 0; r < kConnRounds; ++r) {
            // Grow the vector every round: a push_back reallocates it under the walk. The
            // module name alternates either side of the small-string boundary, so the
            // reallocation moves heap-owning strings the resolver may be copying out, and
            // the KIND is fresh per round — a repeat would now be refused `PATH_IN_USE`
            // (the pair is declared once) and append nothing.
            (void)net.register_module(
                (r & 1) ? "uplink" : "uplink-long-enough-to-leave-the-small-string-buffer",
                std::string(kChurnKind) + std::to_string(r), conn_role_t::DIAL);
            // The pair the resolver thread actually reads, declared ONCE up front so the walk
            // has a hit to find at every reallocation.
            if (r == 0) (void)net.register_module("g", std::string(kChurnKind), conn_role_t::DIAL);
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
    // disturbed it: the last act per name was a removal, and every declaration the declarer
    // appended is still exactly what it appended (nothing rewrites a recorded module now).
    int lingering = 0;
    for (const std::string& q : qualified)
        if (net.settings_of(q) != nullptr) ++lingering;
    check(lingering == 0, "no connection outlived its removal (conns_ survived the churn intact)");

    const auto declared = net.module_for(kChurnKind, conn_role_t::DIAL);
    check(declared.has_value() && *declared == "g",
          "module_for answers the churned pair's one declaration, not a torn string");
}

/**@}*/

}  // namespace

int main() {
    std::printf("control-plane create/remove churn against a live forward reader (ADR-0063)\n");

    graph_t g;
    fwd_router_t router(g);

    // The inbound link is registered once and never churned — the reader needs a stable door.
    sink_t inbound;
    (void)router.add_child("net/in/up", inbound);

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
                (void)router.add_child(name, s);
                (void)router.remove_child(name);
                (void)router.add_child(name, s);
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

    return tr::testing::summary("net_control_plane_race");
}
