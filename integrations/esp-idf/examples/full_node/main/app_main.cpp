/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * full_node — the libtracer FULL-NODE profile on ESP-IDF, in the strawberry
 * shape (strawberry-fw doc/libtracer-migration.md Phase 2 / issue #183):
 *
 *   DEVICE node (what a strawberry board runs)
 *     - ONE static slab feeding both memory seams (ADR-0039 §one-slab):
 *       a `pool_t` region for RX segments (ADR-0042 owning delivery — every
 *       inbound datagram lands in a pool slot, exhaustion is backpressure) and
 *       a `monotonic_buffer_resource` region for the router/label containers
 *       (wrapped in a `synchronized_pool_resource` so recv threads share it);
 *     - a sensor vertex `/sensor/temp` (transient-local, so a fresh subscriber
 *       latches the current value);
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
 *
 * This is example code, not core: plain comments, no Doxygen.
 */

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
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

// The device's UDP listener port (the host peer dials it over loopback).
constexpr std::uint16_t kNodePort = 47301;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// ---- wire builders (canonical bytes via the production emit helpers) --------

view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// SPEC{ NAME "type", NAME "name", SETTINGS "config"{ role, port [, kind][, addr] } }
// — a connection-creation spec (ADR-0027 / reference/05), written to
// /net:children[]. This is THE production wiring path: the connection vertex
// constructs and owns the real socket from this config.
view_t conn_spec(std::string_view type, std::string_view name, conn_role_t role, std::uint16_t port,
                 std::string_view kind, std::string_view addr = {}) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    append(cfg, b_value_u8(static_cast<std::uint8_t>(role)));
    tr::wire::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);
    tr::wire::emit_name(cfg, "kind");
    tr::wire::emit_name(cfg, kind);
    if (!addr.empty()) {
        tr::wire::emit_name(cfg, "addr");
        tr::wire::emit_name(cfg, addr);
    }

    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body, "config");
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

// FIELD{ NAME "subscribers", VALUE u8 index_mode=ELEMENT } — ":subscribers[]" append.
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "subscribers");
    append(body, b_value_u8(1));  // index_mode = ELEMENT (append)
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

// SUBSCRIBER{ PATH target } — the remote-subscriber record a subscribe appends.
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, target);
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

// Decode the u32 out of a stored VALUE TLV (a vertex's last-known value).
std::uint32_t value_u32_of(const view_t& lkv) {
    const auto t = tr::wire::view_as_tlv(lkv);
    if (!t || t->type != type_t::VALUE || t->payload.size() != 4) return 0;
    return tr::detail::load_le<std::uint32_t>(t->payload);
}

// The trailing 4-byte VALUE of a FWD reply (the read's result).
std::uint32_t reply_value_u32(const tr::wire::tlv_t& f) {
    for (auto it = f.children.rbegin(); it != f.children.rend(); ++it)
        if (it->type == type_t::VALUE && it->payload.size() == 4)
            return tr::detail::load_le<std::uint32_t>(it->payload);
    return 0;
}

// ---- the device node (the strawberry shape) ---------------------------------

// ADR-0039 one-slab recipe, concretely: ONE static slab, partitioned once at
// bring-up. The front region becomes the RX segment pool (pool_t — fixed slots,
// exhaustion = backpressure, ADR-0042); the back region backs the container
// memory_resource the router draws its terminus arena and label tables from.
// Steady state allocates from THIS slab, not the global heap.
constexpr std::size_t kSlabBytes = 24 * 1024;
constexpr std::size_t kRxRegion = 12 * 1024;  // pool_t: RX datagram segments
constexpr std::size_t kRxSlotPayload = 1536;  // one UDP/MTU-sized datagram per slot
alignas(std::max_align_t) std::byte g_slab[kSlabBytes];

struct device_node_t {
    // Segment seam: RX datagrams land in pool slots. udp_transport_t sizes its
    // RX segments to the pool's slot payload (min with kMaxDatagram), so
    // MCU-sized slots work as-is.
    tr::mem::pool_t rx_pool{std::span<std::byte>(g_slab, kRxRegion), kRxSlotPayload};
    // Container seam: a monotonic arena over the slab's back region; the
    // synchronized pool on top recycles freed blocks (label tables, terminus
    // arena spill) and makes the resource safe for the recv threads.
    std::pmr::monotonic_buffer_resource arena{g_slab + kRxRegion, kSlabBytes - kRxRegion};
    std::pmr::synchronized_pool_resource mr{&arena};

    graph_t graph;
    fwd_router_t router{graph, &mr};
    // Owns the config-created sockets; declared LAST so its recv threads stop
    // before the router/graph they feed are torn down.
    transport_vertex_t net{graph, router, "/net", &rx_pool};

    tr::graph::vertex_t* sensor = nullptr;

    bool bring_up() {
        // The sensor vertex: transient-local (durability=1) so a fresh remote
        // subscriber LATCHES the current value — one immediate delivery.
        tr::graph::settings_t s;
        s.durability = 1;
        auto reg =
            graph.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE, {}, s);
        if (!reg) return false;
        sensor = *reg;
        if (!write_sensor(21)) return false;

        // The UDP listener, created IN-BAND from config — the production path.
        // NAME "host" is the segment this node prepends to inbound src (the way
        // back) and the segment a dst routes onward through this link.
        const auto w =
            graph.write(*path_t::parse("/net:children[]"),
                        conn_spec("listener", "host", conn_role_t::LISTEN, kNodePort, "udp"));
        return w.has_value();
    }

    bool write_sensor(std::uint32_t v) {
        return graph.write(sensor, owned(b_value_u32(v))).has_value();
    }
};

// ---- the host-peer probe (the self-proof CI runs) ----------------------------

int run_host_probe(device_node_t& dev) {
    std::printf("host peer: dialing the device node over real datagrams (127.0.0.1:%u)\n",
                static_cast<unsigned>(kNodePort));

    // A plain heap-backed peer — this side plays the workstation/UI host.
    graph_t graph;
    fwd_router_t router(graph);
    transport_vertex_t net(graph, router);

    // Fan-out deliveries land here: the device's return route for our subscribe
    // is `src` as the device saw it ({host, self, probe}); the device forwards
    // through its "host" link, we receive {self, probe} and resolve it locally.
    const auto probe_path = *path_t::parse("/self/probe");
    if (!graph.register_vertex(probe_path, role_t::STORED_VALUE)) return 1;

    // The reply sink is installed BEFORE the socket exists (frames may flow the
    // moment the SPEC write returns). No <future>: the example runs under the
    // ESP-IDF default -fno-exceptions, so the capture is a mutex + flag.
    std::mutex reply_m;
    std::vector<std::byte> reply_bytes;
    std::atomic<bool> reply_ready{false};
    router.on_reply([&](const tr::view::rope_t& r) {
        const std::lock_guard lock(reply_m);
        if (!reply_ready.load(std::memory_order_relaxed)) {
            const tr::view::view_t mat = r.materialize();
            const auto b = mat.bytes();
            reply_bytes.assign(b.begin(), b.end());
            reply_ready.store(true, std::memory_order_release);
        }
    });

    // Dial the device: a config-created udp client connection at /net/dev.
    const auto wa =
        graph.write(*path_t::parse("/net:children[]"),
                    conn_spec("client", "dev", conn_role_t::DIAL, kNodePort, "udp", "127.0.0.1"));
    check(wa.has_value(), "SPEC{client, kind=udp, 127.0.0.1} constructs the dialing socket");

    // 1) FWD{READ /dev/sensor/temp} — crosses the wire, resolves at the device
    //    terminus, and the REPLY source-routes back to our reply sink.
    router.on_frame("self", b_fwd(tr::graph::fwd_op_t::READ, b_path({"dev", "sensor", "temp"}),
                                  b_path({"probe"})));
    bool read_ok = false;
    for (int i = 0; i < 60 && !read_ok; ++i) {
        read_ok = reply_ready.load(std::memory_order_acquire);
        if (!read_ok) std::this_thread::sleep_for(50ms);
    }
    std::uint32_t got = 0;
    if (read_ok) {
        const std::lock_guard lock(reply_m);
        const auto dec = tr::wire::decode(reply_bytes);
        got = dec ? reply_value_u32(*dec) : 0;
    }
    check(read_ok && got == 21, "FWD{READ} round-trip: /dev/sensor/temp == 21");

    // 2) Subscribe: a `:subscribers[]` append WRITE binds a REMOTE subscriber at
    //    the device; transient-local latches the current value immediately.
    router.on_frame("self", b_fwd(tr::graph::fwd_op_t::WRITE, b_path({"dev", "sensor", "temp"}),
                                  b_path({"probe"}), b_field_subscribers_append(),
                                  b_subscriber(b_path({"probe"}))));

    // The latch delivery races our next call, so poll-read until it lands.
    bool latched = false;
    for (int i = 0; i < 60 && !latched; ++i) {
        const auto lkv = graph.read(probe_path);
        latched = lkv.has_value() && value_u32_of(lkv->only()) == 21;
        if (!latched) std::this_thread::sleep_for(50ms);
    }
    check(latched, "subscribe latched the current value (transient-local)");

    // 3) Producer fan-out: a plain device-side graph.write fans a FWD{WRITE}
    //    out to us; observed with graph.await on the host side (await is the
    //    consumer's poll — armed BEFORE the write, so no race).
    std::optional<std::uint32_t> delivered;
    std::thread awaiter([&graph, &probe_path, &delivered] {
        const auto d = graph.await(probe_path, 3s);
        if (d.has_value()) delivered = value_u32_of(d->only());
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
        "device node up: /sensor/temp + udp listener /net/host (port %u), one-slab "
        "recipe (pool %u slots x %u B + pmr arena)\n",
        static_cast<unsigned>(kNodePort), static_cast<unsigned>(dev.rx_pool.capacity()),
        static_cast<unsigned>(kRxSlotPayload));

    const int failures = run_host_probe(dev);
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
