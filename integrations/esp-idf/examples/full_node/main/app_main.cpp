/**
 * @file
 * @brief full_node — the libtracer FULL-NODE profile on ESP-IDF, in the
 *        origin-firmware shape.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * full_node — the libtracer FULL-NODE profile on ESP-IDF, in the shape of the
 * originating production firmware, an ESP32-C6 smart-agriculture node (its
 * libtracer-migration plan Phase 2 / issue #183):
 *
 *   DEVICE node (what an origin-firmware board runs)
 *     - ONE static slab feeding both memory seams (ADR-0039 §one-slab):
 *       a synchronised-`pool_t` region for RX segments (ADR-0042 owning
 *       delivery — every
 *       inbound datagram lands in a pool slot, exhaustion is backpressure) and
 *       a `monotonic_buffer_resource` region for the router/label containers
 *       (wrapped in a `synchronized_pool_resource` so recv threads share it);
 *     - a sensor vertex `/sensor/temp` (a subscriber that requests durability
 *       latches the current value — RFC-0022 §3.A);
 *     - `transport_vertex_t` with the built-in udp/tcp/ws transport catalog;
 *       the UDP listener connection is created IN-BAND via a
 *       `write /net:children[] SPEC{listener, kind=udp, port}` — config-created,
 *       exactly how a deployed node is wired;
 *     - a publish loop writing the sensor (each write fans out a real
 *       FWD{WRITE} to every remote subscriber).
 *
 *   HOST-PEER node (the self-proof; what CI runs on the `linux` target)
 *     - a second graph/router/transport_vertex in the same process that DIALS
 *       the device node over REAL datagrams (127.0.0.1 — the kernel/lwIP
 *       loopback, not an in-process shortcut), then drives the consumer
 *       surface end to end: FWD{READ} → reply; `:subscribers[]` subscribe →
 *       latch delivery; device writes → remote fan-out observed via
 *       `graph.await` on the host side.
 *
 * On the `linux` target the app exits 0/1 with the self-proof (the CI gate);
 * on a chip it parks in the publish loop, so a real host on the LAN (Wi-Fi
 * creds via menuconfig) can dial the same listener — the on-silicon e2e.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_sync.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"
#include "platform.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::conn_role_t;
using tr::net::conn_spec;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief The device's UDP listener port (the host peer dials it over loopback). */
constexpr std::uint16_t kNodePort = 47301;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @name wire builders (canonical bytes via the production emit helpers) */

view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> out;
    tr::wire::emit_value_le(out, v);
    return out;
}

std::vector<std::byte> b_value_u8(std::uint8_t v) {
    std::vector<std::byte> out;
    tr::wire::emit_value_le(out, v);
    return out;
}

std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/** @brief FIELD{ NAME "subscribers", VALUE u8 index_mode=ELEMENT } — ":subscribers[]" append. */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "subscribers");
    append(body, b_value_u8(1));  // index_mode = ELEMENT (append)
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief SUBSCRIBER{ PATH target, SETTINGS{ NAME "delivery_policy" VALUE u16 }? } — the
 *        remote-subscriber record a subscribe appends.
 *
 * The optional SETTINGS child carries two independent opt-ins, in the SAME child (so
 * neither introduced new wire structure):
 *
 *   - `delivery_policy` (u16, RFC-0022 §3.A): bits 0–1 reliability, 2–4 priority,
 *     5 `durability_request`, 6–15 reserved;
 *   - `delivery_compact` (u8, RFC-0004 §E.1): ask the producer to advertise a per-link
 *     LABEL for this subscription's return route and then stream lean `COMPACT` frames
 *     instead of re-carrying the whole route on every delivery.
 *
 * Both zero emits no child at all — absent ⇒ all-zero ⇒ the default behaviour,
 * byte-identically to what a sender that predates the keys produces.
 */
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target,
                                    std::uint16_t delivery_policy = 0,
                                    bool delivery_compact = false) {
    std::vector<std::byte> body(target);
    if (delivery_policy != 0 || delivery_compact) {
        std::vector<std::byte> members;
        if (delivery_policy != 0) {
            tr::wire::emit_name(members, "delivery_policy");
            const std::array<std::byte, 2> bits{
                std::byte{static_cast<std::uint8_t>(delivery_policy & 0xFF)},
                std::byte{static_cast<std::uint8_t>(delivery_policy >> 8)}};
            tr::wire::emit_tlv(members, type_t::VALUE, opt_t{}, std::span<const std::byte>(bits));
        }
        if (delivery_compact) {
            tr::wire::emit_name(members, "delivery_compact");
            const std::array<std::byte, 1> one{std::byte{1}};
            tr::wire::emit_tlv(members, type_t::VALUE, opt_t{}, std::span<const std::byte>(one));
        }
        tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, members);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> b_fwd(tr::graph::fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& field = {},
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    append(body, b_value_u8(static_cast<std::uint8_t>(op)));
    append(body, dst);
    if (!field.empty()) append(body, field);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief Decode the u32 out of a stored VALUE TLV (a vertex's last-known value). */
std::uint32_t value_u32_of(const view_t& lkv) {
    const auto t = tr::wire::decode(lkv);
    if (!t || t->type != type_t::VALUE || t->payload.size() != 4) return 0;
    return tr::detail::load_le<std::uint32_t>(t->payload);
}

/** @brief The trailing 4-byte VALUE of a FWD reply (the read's result). */
std::uint32_t reply_value_u32(const tr::wire::tlv_t& f) {
    for (auto it = f.children.rbegin(); it != f.children.rend(); ++it)
        if (it->type == type_t::VALUE && it->payload.size() == 4)
            return tr::detail::load_le<std::uint32_t>(it->payload);
    return 0;
}

/** @name the device node (the origin-firmware shape) */

/**
 * @brief ADR-0039 one-slab recipe, concretely: ONE static slab, partitioned
 *        once at bring-up.
 *
 * THREE regions, each behind the seam that owns it:
 *
 * | region | seam | what it bounds |
 * | --- | --- | --- |
 * | front, 12 KiB | a synchronised `pool_t` (`rx_backend`) | RX datagram segments — fixed slots,
 * exhaustion = backpressure (ADR-0042) | | middle, 2 KiB | a `tr::mem::pool_source_t` | the
 * router's LABEL TABLES (#603 defect 1) | | back, 10 KiB | `monotonic_buffer_resource` +
 * `synchronized_pool_resource` | the remaining pmr containers (LKV control blocks) |
 *
 * The label region is new, and it exists to KEEP a bound this example already
 * advertised rather than to add one. The label tables used to draw from the pmr
 * resource in the back region; since #603 defect 1 they draw from an injected
 * `tr::mem::block_source_t`, because a `std::pmr::memory_resource` cannot report
 * exhaustion by value and a peer's `ADVERTISE` reaches this store on a receive thread,
 * pre-ACL — on `-fno-exceptions` that was a peer-triggerable reboot. Leaving
 * `label_src` at its default would have quietly moved the label tables OUT of the slab,
 * which is the opposite of what this example is for.
 *
 * It is a `pool_source_t` and not a `bump_source_t` for the reason ADR-0067 §1 gives:
 * label state is LONG-LIVED and churns (`clear_link` frees a whole link's tables on
 * every reconnect), and a bump source never reclaims, so it would serve a few link
 * flaps and then refuse every one after. Both of its bounds are injected — the slab
 * span AND the `size_class_t` span — and both are REPORTED at bring-up
 * (`used`/`classes_used`/`overflowed`), so the sizing below is a measurement to check
 * rather than a constant to trust.
 *
 * STILL not slab-bound (#588): the terminus decode ARENA draws from the router's `rx`
 * block source, which this example leaves at the default heap, as are the graph's own
 * three seams (the example default-constructs the graph). Bounding those is a separate
 * follow-on — ADR-0067 §3 wants a PER-CHILD source there rather than one shared across
 * receive threads, which is a different topology from the single shared source the
 * label plane wants. See docs/reference/09 §the second L0 seam and
 * docs/interop/esp32-production-node.md.
 */
constexpr std::size_t kSlabBytes = 24 * 1024;
constexpr std::size_t kRxRegion = 12 * 1024; /**< @brief Synchronised pool: RX datagram segments. */
constexpr std::size_t kRxSlotPayload = 1536; /**< @brief One UDP/MTU-sized datagram per slot. */
constexpr std::size_t kLabelRegion = 2 * 1024; /**< @brief Recycling source: the label tables. */
/**
 * @brief Free-list slots for the label source — one per distinct `(bytes, align)` shape.
 *
 * `pool_source_t` segregates by EXACT size, so the count to size this against is the
 * number of shapes the label plane draws, not the number of blocks. Running out is safe
 * but lossy (a freed block stops being recycled), which is why `overflowed()` is
 * printed at bring-up: a non-zero reading there is the signal to raise this number.
 */
constexpr std::size_t kLabelClasses = 12;
alignas(std::max_align_t) std::byte g_slab[kSlabBytes];

/** @brief The device node: the one-slab substrate, graph, router, and transport vertex. */
struct device_node_t {
    /**
     * @brief Segment seam: RX datagrams land in pool slots — a SYNCHRONISED pool.
     *
     * udp_transport_t sizes its RX segments to the pool's slot payload (min
     * with kMaxDatagram), so MCU-sized slots work as-is.
     *
     * It is NOT a bare `tr::mem::pool_t` (#770): this backend is handed to every
     * endpoint, each allocates on its own receive thread, and a delivered segment
     * reclaims on whichever thread drops the last reference — a plain free list is
     * two-threads-one-slot there. Which critical section guards it is a per-target
     * compile-time policy behind `rx_backend()` (platform.hpp): the interrupt-disable
     * one on a chip, a spinlock on the host. Same slab, same bound (ADR-0060 §2).
     */
    tr::mem::mem_backend_t& rx_pool =
        rx_backend(std::span<std::byte>(g_slab, kRxRegion), kRxSlotPayload);
    /**
     * @brief Failable seam: the router's LABEL TABLES, bounded by the slab's middle region.
     *
     * A `tr::mem::pool_source_t` — bounded, RECYCLING, and nothrow: exhaustion is a
     * `nullptr` the store answers by refusing to compact a NEW flow, which degrades it to
     * the full-route `FWD{WRITE}` form and leaves every established flow alone. That is
     * what makes an `ADVERTISE` storm from a peer a throughput event on this node instead
     * of a reboot (#603 defect 1).
     *
     * `sync_mutex_t`, not the interrupt-disable policy the RX pool uses, and the
     * difference is the frequency and the context. The RX pool is touched from an ISR and
     * on every datagram, so it needs `tr::esp::portmux_sync_t` on a chip. This source is
     * touched only when a FLOW IS SET UP — `on_advertise` learning a binding on a receive
     * thread, `ensure_egress` MINTING a label on the writer thread; the per-delivery reuse
     * path finds the label already bound and reaches no allocator at all. That is exactly
     * the "wiring frequency" case `sync_mutex_t` documents itself for, and it is portable
     * across both of this example's targets, so it needs no platform seam of its own.
     */
    std::array<tr::mem::size_class_t, kLabelClasses> label_classes{};
    tr::mem::pool_source_t<tr::mem::sync_mutex_t> label_src{
        std::span<std::byte>(g_slab + kRxRegion, kLabelRegion), label_classes};
    /**
     * @brief Container seam: a monotonic arena over the slab's back region.
     *
     * The synchronized pool on top recycles freed blocks and makes the resource safe for
     * the recv threads. Two things are NOT among them: the terminus arena (since #588 it
     * draws from the router's `rx` block source, left at the default heap here) and the
     * label tables (since #603 defect 1 they draw from `label_src` above — a pmr resource
     * cannot report exhaustion by value, which is the whole defect).
     */
    std::pmr::monotonic_buffer_resource arena{g_slab + kRxRegion + kLabelRegion,
                                              kSlabBytes - kRxRegion - kLabelRegion};
    std::pmr::synchronized_pool_resource mr{&arena};

    graph_t graph;
    fwd_router_t router{graph, &label_src};
    /**
     * @brief Owns the config-created sockets; declared LAST so its recv
     *        threads stop before the router/graph they feed are torn down.
     */
    transport_vertex_t net{graph, router, "/net", &rx_pool};

    std::optional<tr::graph::vertex_handle_t> sensor;

    bool bring_up() {
        // ADR-0073 §4 (declared-only): the APPLICATION mints the module its udp
        // listener mounts under — the library registers no module names. This
        // node adopts the built-in's suggested name.
        if (!net.register_module(std::string(tr::net::kUdpServerSuggestedModule), "udp",
                                 conn_role_t::LISTEN))
            return false;
        // The sensor vertex: a plain producer. Since RFC-0022 §3.A a fresh remote
        // subscriber LATCHES the current value by ASKING for it (the SUBSCRIBER's
        // `delivery_policy` bit 5), so the producer carries no durability flag.
        sensor = graph.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
        if (!write_sensor(21)) return false;

        // The UDP listener, created IN-BAND from config — the production path.
        // NAME "host" is the segment this node prepends to inbound src (the way
        // back) and the segment a dst routes onward through this link.
        const auto w =
            graph.write(path_t("/net:children[]"),
                        conn_spec("listener", "host", conn_role_t::LISTEN, kNodePort, "udp"));
        return w.has_value();
    }

    bool write_sensor(std::uint32_t v) {
        return graph.write(*sensor, owned(b_value_u32(v))).has_value();
    }
};

/** @name the host-peer probe (the self-proof CI runs) */

int run_host_probe(device_node_t& dev) {
    std::printf("host peer: dialing the device node over real datagrams (127.0.0.1:%u)\n",
                static_cast<unsigned>(kNodePort));

    // A plain heap-backed peer — this side plays the workstation/UI host.
    graph_t graph;
    fwd_router_t router(graph);
    transport_vertex_t net(graph, router);
    // ADR-0073 §4 (declared-only): this host-peer application mints the module its
    // udp dial link mounts under (the built-in's suggested name).
    (void)net.register_module(std::string(tr::net::kUdpClientSuggestedModule), "udp",
                              conn_role_t::DIAL);

    // Fan-out deliveries land here: the device's return route for our subscribe
    // is `src` as the device saw it ({host, self, probe}); the device forwards
    // through its "host" link, we receive {self, probe} and resolve it locally.
    const auto probe_path = path_t("/self/probe");
    (void)graph.register_vertex(probe_path, role_t::STORED_VALUE);  // infallible (ADR-0056)

    // The reply sink is installed BEFORE the socket exists (frames may flow the
    // moment the SPEC write returns). No <future>: the example runs under the
    // ESP-IDF default -fno-exceptions, so the capture is a mutex + flag.
    struct reply_box_t {
        std::mutex m;
        std::vector<std::byte> bytes;
        std::atomic<bool> ready{false};
    } reply_box;
    router.on_reply(
        [](void* ctx, const tr::view::rope_t& r) {
            auto* box = static_cast<reply_box_t*>(ctx);
            const std::lock_guard lock(box->m);
            if (!box->ready.load(std::memory_order_relaxed)) {
                const tr::view::view_t mat = r.materialize();
                const auto b = mat.bytes();
                box->bytes.assign(b.begin(), b.end());
                box->ready.store(true, std::memory_order_release);
            }
        },
        &reply_box);

    // Dial the device: a config-created udp client connection at /net/udp-client/dev.
    const auto wa =
        graph.write(path_t("/net:children[]"),
                    conn_spec("client", "dev", conn_role_t::DIAL, kNodePort, "udp", "127.0.0.1"));
    check(wa.has_value(), "SPEC{client, kind=udp, 127.0.0.1} constructs the dialing socket");

    // 1) FWD{READ /dev/sensor/temp} — crosses the wire, resolves at the device
    //    terminus, and the REPLY source-routes back to our reply sink.
    router.on_frame(
        "self", b_fwd(tr::graph::fwd_op_t::READ,
                      b_path({"net", "udp-client", "dev", "sensor", "temp"}), b_path({"probe"})));
    bool read_ok = false;
    for (int i = 0; i < 60 && !read_ok; ++i) {
        read_ok = reply_box.ready.load(std::memory_order_acquire);
        if (!read_ok) std::this_thread::sleep_for(50ms);
    }
    std::uint32_t got = 0;
    if (read_ok) {
        const std::lock_guard lock(reply_box.m);
        const auto dec = tr::wire::decode(reply_box.bytes);
        got = dec ? reply_value_u32(*dec) : 0;
    }
    check(read_ok && got == 21, "FWD{READ} round-trip: /dev/sensor/temp == 21");

    // 2) Subscribe: a `:subscribers[]` append WRITE binds a REMOTE subscriber at the
    //    device. This subscription REQUESTS durability (RFC-0022 §3.A bit 5), so the
    //    producer latches its current value to it immediately — the producer carries no
    //    durability flag of its own, and a sibling subscriber that does not ask gets no
    //    replay.
    //
    //    It ALSO opts into label compaction (RFC-0004 §E.1). That is what a streaming
    //    consumer of a 1 kHz sensor does — re-carrying the whole return route on every
    //    4-byte sample is ~16x overhead — and it is what makes the device's LABEL SOURCE
    //    live: the producer advertises a per-link label for this route and thereafter
    //    streams lean COMPACTs, and the label state that costs is drawn from the bounded
    //    `label_src` region of the one slab. Without this bit the census printed at the
    //    end would read 0 B used and prove nothing about the bound.
    router.on_frame(
        "self",
        b_fwd(tr::graph::fwd_op_t::WRITE, b_path({"net", "udp-client", "dev", "sensor", "temp"}),
              b_path({"probe"}), b_field_subscribers_append(),
              b_subscriber(b_path({"probe"}), tr::graph::delivery_policy_t::kDurabilityRequest,
                           /*delivery_compact=*/true)));

    // The latch delivery races our next call, so poll-read until it lands.
    bool latched = false;
    for (int i = 0; i < 60 && !latched; ++i) {
        const auto lkv = graph.read(probe_path);
        latched = lkv.has_value() && value_u32_of((*lkv)->only()) == 21;
        if (!latched) std::this_thread::sleep_for(50ms);
    }
    check(latched, "a durability_request subscribe latched the current value");

    // 3) Producer fan-out: a plain device-side graph.write fans a FWD{WRITE}
    //    out to us; observed with graph.await on the host side (await is the
    //    consumer's poll — armed BEFORE the write, so no race).
    std::optional<std::uint32_t> delivered;
    std::thread awaiter([&graph, &probe_path, &delivered] {
        const auto d = graph.await(probe_path, 3s);
        if (d.has_value()) delivered = value_u32_of((*d)->only());
    });
    std::this_thread::sleep_for(100ms);  // let await arm
    check(dev.write_sensor(22), "device: write /sensor/temp = 22");
    awaiter.join();
    check(delivered.has_value() && *delivered == 22,
          "remote fan-out delivered 22 to the host peer (await fired)");

    return g_failures;
}

}  // namespace

extern "C" void app_main(void) {
    std::printf("libtracer full node (ESP-IDF) starting\n");

    if (!platform_bring_up()) {
        std::printf("FAIL: platform bring-up\n");
        std::exit(1);
    }

    // The node is intentionally LEAKED, never destroyed: on a device it parks
    // in the publish loop forever, and on the linux target std::exit must not
    // run destructors underneath live recv threads (a static here would).
    device_node_t& dev = *new device_node_t;
    if (!dev.bring_up()) {
        std::printf("FAIL: device node bring-up\n");
        std::exit(1);
    }
    std::printf(
        "device node up: /sensor/temp + udp listener /net/udp-server/host (port %u), one-slab "
        "recipe (pool %u slots x %u B + label source %u B + pmr arena)\n",
        static_cast<unsigned>(kNodePort), static_cast<unsigned>(rx_backend_slots()),
        static_cast<unsigned>(kRxSlotPayload), static_cast<unsigned>(kLabelRegion));

    const int failures = run_host_probe(dev);
    // The label source's own census, AFTER the self-proof has driven real advertises and
    // compact deliveries through it. Both of its bounds are injected, so both are reported:
    // a `used` near the region size means raise `kLabelRegion`, a non-zero `overflowed`
    // means raise `kLabelClasses`. Printing them is what makes the sizing above a
    // measurement rather than a guess, and it is the same discipline `rx_backend_slots()`
    // already applies to the RX region.
    std::printf("label source: %u/%u B used, %u/%u size classes, %u block(s) overflowed\n",
                static_cast<unsigned>(dev.label_src.used()), static_cast<unsigned>(kLabelRegion),
                static_cast<unsigned>(dev.label_src.classes_used()),
                static_cast<unsigned>(kLabelClasses),
                static_cast<unsigned>(dev.label_src.overflowed()));
    std::printf("full_node self-proof: %s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED",
                failures, failures == 1 ? "" : "s");

    if (!platform_is_device()) std::exit(failures == 0 ? 0 : 1);

    // Real device: park in the publish loop — every write fans out to whatever
    // remote subscribers are bound (a LAN host can dial the same listener).
    std::uint32_t v = 23;
    while (true) {
        (void)dev.write_sensor(v++);
        std::this_thread::sleep_for(1s);
    }
}
