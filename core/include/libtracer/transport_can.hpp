/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_can (increment 2 of #55) — the SocketCAN binding that drives the
 * pure framing layer (can.hpp / view_can.hpp / can_reassembly.hpp) over a
 * real Linux CAN bus. It is a `tr::net::transport_t`: a bridge hands it a complete
 * libtracer frame via send(), the transport address-shift-fragments that frame
 * across CAN data fields (header-elided — the 29-bit CAN ID is the path, ADR-0022),
 * writes them to the wire, and on the way back reassembles the slices and delivers
 * the byte-exact frame to the registered receiver. The identity↔path map lives
 * INSIDE the transport and self-establishes from in-band `advertise` frames
 * (ADR-0030) — pure-decentralized, no gateway.
 *
 * The raw frame I/O sits behind the `can_link_t` seam so the transport is testable
 * with no kernel CAN: `socketcan_link_t` is the production `PF_CAN`/`SOCK_RAW`
 * impl, while tests pair two transports over an in-memory fake link (see
 * core/tests/transport_can_test.cpp). The transport itself never touches a socket.
 *
 * Concurrency mirrors transport_ws: the link's receive loop runs on an internal
 * thread and feeds the bridge through the registered receiver; sends are serialized
 * so one group's frames never interleave another's on the bus.
 */
#pragma once

#include <pthread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/can.hpp"
#include "libtracer/can_reassembly.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/view_can.hpp"

/**
 * @file
 * @brief The SocketCAN binding `tr::net::transport_can`, its raw-frame seam
 *        `can_link_t`, and the production `socketcan_link_t`.
 */

namespace tr::net {

/**
 * @brief The `endpoint` slot reserved for the in-band `advertise` control stream.
 *
 * Advertise frames ride a single CAN ID per node (`[version|node|0]`). It is the
 * numerically lowest endpoint, so on a contended bus the control stream wins
 * arbitration over the data slots and a binding's manifest reaches peers ahead of
 * the lean data frames it governs.
 */
inline constexpr std::uint16_t kCanControlEndpoint = 0;

/** @brief The first `endpoint` slot usable for header-elided data groups (control is 0). */
inline constexpr std::uint16_t kCanFirstDataEndpoint = 1;

/**
 * @brief The largest address-shift group this node can place: every data endpoint slot.
 *
 * Address-shift slicing spreads a group across CONSECUTIVE endpoint slots of one node
 * (`endpoint[base .. base+count-1]`), so a group of more slices than the data-endpoint
 * window holds can never be placed at any base — no wrap helps. DERIVED from the CAN-ID
 * field widths (`can::kEndpointMax`) minus the reserved control slot, never a chosen
 * number: the bound is the wire's, so widening `kEndpointBits` widens this with it.
 *
 * A group over this bound is refused WHOLE, before its manifest is emitted (#910) —
 * advertising `slice_count` slices and then running out of slots mid-loop leaves every
 * receiver holding a reassembly group that can never complete.
 */
inline constexpr std::size_t kCanMaxGroupSlices =
    static_cast<std::size_t>(can::kEndpointMax) - kCanFirstDataEndpoint + 1u;

/** @brief Default liveness window: a peer silent this long leaves the enumeration. */
inline constexpr std::chrono::milliseconds kCanDefaultPeerTtl{3000};

/**
 * @brief Sentinel for `transport_can_config_t::rx_ttl`: track `peer_ttl` instead.
 *
 * The RX staleness window is not an independent quantity to invent a number for
 * (the no-synthetic-limits rule): a peer that has been silent longer than
 * `peer_ttl` is already considered gone, so RX state it would have completed is
 * definitively dead by then. Left at zero, `rx_ttl` resolves to `peer_ttl`.
 */
inline constexpr std::chrono::milliseconds kCanRxTtlFromPeerTtl{0};

/**
 * @brief One raw CAN frame at the `can_link_t` seam — id + data field, no semantics.
 *
 * A mode-agnostic carrier for both a classic CAN 2.0B frame (`fd == false`,
 * `len <= 8`) and a CAN-FD frame (`fd == true`, `len` a valid DLC size up to 64).
 * The transport plane fills this in; the link lowers it to the kernel `struct
 * can_frame` / `struct canfd_frame` (or, in tests, an in-memory queue).
 */
struct can_frame_data_t {
    std::uint32_t id = 0; /**< @brief The 29-bit extended CAN identifier. */
    bool fd = false;      /**< @brief True ⇒ a CAN-FD frame; false ⇒ classic CAN 2.0. */
    std::uint8_t len = 0; /**< @brief Data-field length on the wire (post-DLC-pad for FD). */
    std::array<std::byte, tr::view::kCanFdMaxData>
        data{}; /**< @brief The data field; only the first @ref len bytes are live. */

    /** @brief The live data-field bytes as a read-only span. */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>(data.data(), len);
    }
};

/**
 * @brief The largest data field a frame of this mode may declare, as a @ref
 *        can_frame_data_t::len.
 *
 * The widths themselves are the L1 framing layer's (@ref tr::view::can_max_data —
 * 8 classic, 64 FD, facts of the wire rather than chosen bounds). This is only the
 * seam's adapter to them: the carrier spells its mode as a `bool` and its length as
 * a `std::uint8_t`, so the bound arrives in the same width as the field it bounds and
 * no second copy of the numbers lives here.
 */
[[nodiscard]] constexpr std::uint8_t can_max_len(bool fd) noexcept {
    return static_cast<std::uint8_t>(tr::view::can_max_data(
        fd ? tr::view::can_frame_mode_t::FD : tr::view::can_frame_mode_t::CLASSIC));
}

/**
 * @brief Is an inbound raw frame admissible at the @ref can_link_t seam?
 *
 * The ONE ingress admission rule, so the platform links cannot drift on which
 * frames they let through (#931). Each link decodes the three flags from its own
 * driver's representation — `socketcan_link_t` from the `CAN_EFF_FLAG` /
 * `CAN_RTR_FLAG` / `CAN_ERR_FLAG` bits of `can_id`, the ESP-IDF component's
 * `twai_link_t` from `twai_frame_t::header.ide` / `.rtr` — and the verdict is
 * decided here, once.
 *
 * The binding is header-elided: the 29-bit **extended** identifier IS the path
 * (ADR-0022), so a standard 11-bit frame carries no decodable identity. A
 * remote-transmission request carries a DLC but **no** data bytes, and an error
 * frame is a controller status report rather than bus payload — admitting either
 * hands the reassembler a slice whose length is a lie, and the flags that told it
 * apart are stripped by the time the id reaches @ref can_frame_data_t::id.
 *
 * @param extended The frame uses a 29-bit extended identifier, not 11-bit standard.
 * @param remote   The frame is a remote-transmission request (RTR).
 * @param error    The frame is a controller error/status frame, not bus traffic.
 */
[[nodiscard]] constexpr bool can_rx_admissible(bool extended, bool remote, bool error) noexcept {
    return extended && !remote && !error;
}

/**
 * @brief Is @p frame emittable at the @ref can_link_t seam?
 *
 * The egress half of the same shared rule: a declared length must fit the data
 * field the frame's mode actually has. A link that skipped it would `memcpy`
 * @ref can_frame_data_t::len bytes out of a 64-byte carrier into an 8-byte kernel
 * `struct can_frame::data` — a stack smash the seam precondition alone was
 * holding back (#931).
 */
[[nodiscard]] constexpr bool can_tx_admissible(const can_frame_data_t& frame) noexcept {
    return frame.len <= can_max_len(frame.fd);
}

/**
 * @brief The raw-frame seam between @ref transport_can and a physical CAN bus.
 *
 * Abstracting the socket here is what makes @ref transport_can testable without
 * the kernel `vcan` module: production uses @ref socketcan_link_t, tests use an
 * in-memory paired link. A link is single-owner (held by one transport) and
 * delivers inbound frames through the registered @ref rx_fn_t, which may fire on
 * an internal receive thread.
 *
 * **Two-phase lifecycle (#1186).** Construction only OPENS the link — it must not
 * begin reading. Inbound frames start flowing at @ref start, which the owner calls
 * after @ref on_receive has been registered, so there is no window in which the
 * link reads a frame with no sink to hand it to. Before this the receive thread was
 * spawned by the constructor and the ordering requirement lived only in prose, which
 * no well-behaved caller could satisfy; the two-phase shape makes it compile-checked
 * for the implementer and explicit for the owner. Egress does NOT wait on @ref start —
 * @ref write_raw is live as soon as the link is open.
 *
 * **Which frames cross the seam is the seam's own rule, not each link's.** Every
 * port of a *physical* bus gates ingress on @ref can_rx_admissible and egress on
 * @ref can_tx_admissible, so a bus-visible divergence between two ports is a
 * compile-unit-local bug rather than a design choice (#931). In-memory test links
 * are exempt by construction — their carrier cannot express RTR, an 11-bit
 * identifier, or an error flag, so the ingress rule has no input to judge, and one
 * of them injects raw fragments deliberately.
 */
class can_link_t {
   public:
    /** @brief Callback invoked once per inbound raw CAN frame (may run off-thread). */
    using rx_fn_t = std::function<void(const can_frame_data_t&)>;

    virtual ~can_link_t() = default;

    /**
     * @brief Emit one raw CAN frame onto the bus.
     *
     * @p frame is only BORROWED for the duration of the call. An
     * implementation whose driver transmits asynchronously (queues the frame
     * pointer and formats the buffer later, possibly from a tx-done ISR —
     * e.g. ESP-IDF's `esp_driver_twai` behind the component's `twai_link_t`)
     * must copy the frame into storage the LINK owns until the driver signals
     * completion (#383; `%can_tx_pool.hpp` is that storage). A synchronous
     * link (`socketcan_link_t`: the kernel copies inside the write(2) call)
     * may use @p frame directly.
     */
    virtual void write_raw(const can_frame_data_t& frame) = 0;

    /** @brief Register the sink for inbound raw frames; set before @ref start. */
    virtual void on_receive(rx_fn_t rx) = 0;

    /**
     * @brief Begin reading from the bus — the second phase of the lifecycle (#1186).
     *
     * Spawns whatever machinery delivers inbound frames (a receive thread, a
     * dispatch task) and is the FIRST point at which @ref rx_fn_t can fire. Call
     * it once, after @ref on_receive; a second call is a no-op, and a link that
     * failed to open stays silent rather than reporting an error — this seam has
     * no error channel, and the open-time predicate (`ok()` on the ports that have
     * one) already answers whether the link came up. Frames that arrived between
     * open and this call are not lost: the kernel socket buffer / driver queue
     * holds them until the first read.
     */
    virtual void start() = 0;
};

/**
 * @brief The production `can_link_t` — a real Linux SocketCAN `PF_CAN` socket.
 *
 * Opens a `socket(PF_CAN, SOCK_RAW, CAN_RAW)`, enables CAN-FD frames
 * (`CAN_RAW_FD_FRAMES`, best-effort — a classic-only controller still works),
 * binds to the named interface (e.g. `"vcan0"`/`"can0"`); @ref start then spawns
 * the receive thread that translates each kernel frame into a
 * @ref can_frame_data_t for the registered callback. The implementation is selected by the BUILD
 * SYSTEM, not by macros: Linux compiles `src/socketcan_link.cpp` (`<linux/can.h>`), every other
 * platform compiles `src/socketcan_link_stub.cpp`, whose @ref ok is always false — so
 * sanitizer/non-Linux builds stay clean and platform ports (e.g. the ESP-IDF component's
 * `twai_link_t`) implement @ref can_link_t in their own TU. The send path is
 * `MSG_NOSIGNAL`-equivalent (a raw CAN write cannot SIGPIPE) and serialized; the fd is reset under
 * the write lock before close on shutdown.
 */
class socketcan_link_t : public can_link_t {
   public:
    /**
     * @brief Open + bind a `CAN_RAW` socket on interface @p ifname — no reading yet.
     *
     * The socket is live for TX on return; the receive thread is spawned by
     * @ref start, not here (#1186).
     * @param ifname The CAN network interface name (e.g. `"vcan0"`).
     * @param recv_stack Receive-thread stack size in bytes, 0 = platform default.
     *        Non-zero right-sizes the dispatch thread on an MCU (applied via
     *        `pthread_attr_setstacksize` at @ref start; mirrors
     *        `net::posix_endpoint_t::start`).
     */
    explicit socketcan_link_t(const std::string& ifname, std::size_t recv_stack = 0);

    /** @brief Stop the receive thread and close the socket. */
    ~socketcan_link_t() override;

    socketcan_link_t(const socketcan_link_t&) = delete;
    socketcan_link_t& operator=(const socketcan_link_t&) = delete;

    /** @brief Write one frame to the bus (classic or FD per @ref can_frame_data_t::fd). */
    void write_raw(const can_frame_data_t& frame) override;

    /** @brief Register the inbound-frame sink (invoked on the receive thread). */
    void on_receive(rx_fn_t rx) override;

    /**
     * @brief Spawn the receive thread — call after @ref on_receive (#1186).
     *
     * A no-op when the socket never opened or the thread is already running.
     * Frames the kernel buffered since the bind are read by the first iteration,
     * so the two-phase split loses nothing.
     */
    void start() override;

    /** @brief True if the socket opened and bound (false on non-Linux or any error). */
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

   private:
    void run();                             // receive thread
    static void* thread_entry(void* self);  // pthread trampoline → run()

    int fd_ = -1;
    std::size_t recv_stack_ = 0;  // receive-thread stack hint, applied at start()
    rx_fn_t rx_;                  // guarded by m_
    std::mutex m_;                // guards rx_
    std::mutex write_m_;          // serializes writes / fd teardown
    std::atomic<bool> stop_{false};
    // pthread (not std::thread): its ctor throws on failure → std::abort under
    // -fno-exceptions; pthread_create returns an error code (see posix_endpoint).
    pthread_t thread_{};    // valid only while started_
    bool started_ = false;  // whether thread_ holds a joinable thread
};

/**
 * @brief Static identity of a @ref transport_can node on the bus.
 *
 * Fixes the CAN-ID `version`/`node` band this transport transmits in and the
 * framing mode it slices into. @ref path is the libtracer path this node binds in
 * its outbound `advertise` manifests (the `id ↔ path` the map establishes).
 */
struct transport_can_config_t {
    std::uint8_t version = 0; /**< @brief Protocol-version prefix (discovery-layer versioning). */
    std::uint16_t node = 0;   /**< @brief This node's id (the CAN-ID `node` band). */
    tr::view::can_frame_mode_t mode =
        tr::view::can_frame_mode_t::CLASSIC; /**< @brief Classic (≤8B) or CAN-FD (≤64B) framing. */
    std::string path; /**< @brief The path advertised for this node's groups. */
    std::chrono::milliseconds peer_ttl =
        kCanDefaultPeerTtl; /**< @brief Peer liveness window (ADR-0044): a peer silent
                                 longer than this expires from the enumeration. */

    // --- ingress bounding (#912): the injected-resource / config seam ----------

    std::pmr::memory_resource* reasm_mr =
        std::pmr::new_delete_resource(); /**< @brief Where the RX buffers (reassembly
                                              groups/slices and the pending-slice queue)
                                              draw their structure. A constrained node
                                              injects a bounded resource; the default is
                                              the process heap. Must outlive the
                                              transport. */
    std::size_t max_groups = 0;          /**< @brief Live reassembly-group ceiling; `0` = unbounded
                                              (host-bounded per RFC-0006). Overflow evicts the
                                              oldest group and ticks @ref
                                              transport_can::dropped_groups. */
    std::size_t max_pending = 0;         /**< @brief Ceiling on data slices parked awaiting their
                                              advertise; `0` = unbounded (host-bounded per
                                              RFC-0006). Overflow evicts the oldest parked slice
                                              and ticks @ref transport_can::dropped_rx. */
    std::chrono::milliseconds rx_ttl =
        kCanRxTtlFromPeerTtl; /**< @brief RX staleness window: a parked slice or an
                                   incomplete reassembly group untouched this long is
                                   reclaimed, because a lost advertise/data slice would
                                   otherwise pin it forever. `0` = track `peer_ttl`
                                   (@ref kCanRxTtlFromPeerTtl); if `peer_ttl` is itself
                                   `0` the window stays `0`, which retains only what
                                   arrived this instant — the same reading the peer
                                   enumeration gives that value, never "disabled".
                                   Unlike the count caps this is ALWAYS live — the
                                   age-out is the bound that holds under the shipped
                                   default config. */
    mem::mem_backend_t* rx_backend =
        nullptr; /**< @brief The byte seam an inbound data slice is COPIED into before it
                      enters the reassembly buffer (`tr::view::over_bytes`'s injected
                      form, #793). `nullptr` = the process heap, which is what this path
                      used unconditionally before #911. A constrained node injects a
                      bounded backend (`mem::pool_t`) so ingress exhaustion is a
                      by-value refusal on the RX thread instead of a reach into the
                      global heap; a refusal drops the whole group and ticks @ref
                      transport_can::dropped_rx. Must outlive the transport — the
                      segments it hands out are released by it. Companion to @ref
                      reasm_mr — that one bounds the reassembly STRUCTURE, this one the
                      slice BYTES. */
};

/**
 * @brief A `transport_t` over Linux SocketCAN — header-elided, self-establishing.
 *
 * Wires the increment-1 framing to a live bus. **Egress** (@ref send): the frame
 * is address-shift-fragmented by @ref tr::view::can_frame_at into CAN data
 * fields, an in-band @ref tr::net::can::advertise_t manifest (carrying the slice
 * count and exact total length) is emitted on the control ID, then the lean
 * id-matched data frames follow — CAN-FD windows DLC-padded up to a legal size.
 * **Ingress** (the link's receive thread): advertise frames populate the dynamic
 * identity↔path map; data frames are reassembled by @ref
 * can_reassembly_t keyed by `(node, base-endpoint) + slice-index`,
 * trimmed back to the advertised total (undoing FD padding), and delivered
 * byte-exact to the receiver. The map is rebuilt purely from advertise frames, so
 * a rejoining node self-heals with no coordinator (ADR-0030). The base endpoint
 * RECURS — the 12-bit space wraps — so that key is only unambiguous because a
 * fresh advertise retires every binding whose endpoint run it overlaps, and the
 * group each was feeding with it (#909, `invalidate_overlapping`).
 *
 * **Bus capability (ADR-0044).** The bus reaches many peers over one wire, so the
 * transport also implements @ref bus_link_t — statelessly, from live traffic:
 *  - a last-heard table (one entry per DISTINCT node id ever heard — like the
 *    identity↔path map, it grows with the bus population, structurally bounded
 *    by the 13-bit node-id space, never per-request/per-frame; memory policy is
 *    the host's) is refreshed by every valid same-version frame another node
 *    emits, seeded by the **hello** advertise (`slice_count == 0`) a node sends
 *    at join; a peer silent longer than `peer_ttl` expires from view;
 *  - @ref enumerate_peers synthesizes the currently-audible peer names —
 *    `n<node-id>` (decimal, no leading zeros): deterministic and collision-safe
 *    within the bus, since the structured CAN ID makes node ids unique per bus;
 *  - @ref peer_link resolves such a name to a per-peer DIRECTED endpoint whose
 *    `send` stamps the group's advertise with `target_node`, so on the broadcast
 *    medium only the addressed peer reassembles and delivers it;
 *  - @ref set_peer_receiver tags each delivered frame with the SENDER's peer
 *    name (derived from the CAN ID), which the FWD router uses as the hop's
 *    inbound NAME — replies route back per-peer with no per-request state.
 * No peer ever creates a vertex or any other graph state (ADR-0044 §1).
 */
class transport_can : public transport_t, public bus_link_t {
   public:
    /**
     * @brief Bind this transport to raw link @p link with node identity @p config.
     *
     * Drives the link's two-phase lifecycle (#1186) on the owner's behalf:
     * registers the receiver, THEN calls @ref can_link_t::start. A caller that
     * hands its link here must not have started it.
     * @param link   The owned raw-frame link (a @ref socketcan_link_t in production),
     *               open but not yet started.
     * @param config This node's version/node/mode/path identity on the bus.
     */
    transport_can(std::unique_ptr<can_link_t> link, transport_can_config_t config);

    /** @brief Detach the receiver and release the link (stopping its receive thread). */
    ~transport_can() override;

    transport_can(const transport_can&) = delete;
    transport_can& operator=(const transport_can&) = delete;

    /**
     * @brief Fragment @p frame across CAN frames and emit it (advertise + data).
     *
     * Empty frames are dropped. Thread-safe: a whole group (its advertise and data
     * frames) is emitted under one lock so concurrent sends never interleave.
     * @param frame A complete libtracer frame (a ROUTER-wrapped TLV's bytes).
     */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Look up a learned `id ↔ path` binding by its base CAN ID (test/introspection hook).
     * @param base_can_id The advertised group's base 29-bit CAN ID.
     * @return The learned @ref tr::net::can::advertise_t, or `std::nullopt` if unknown.
     */
    [[nodiscard]] std::optional<can::advertise_t> learned_binding(std::uint32_t base_can_id) const;

    // --- drop counters (#912) — the sibling-transport convention -----------------

    /**
     * @brief Inbound frames dropped rather than delivered (the tcp/quic/udp
     *        `dropped_rx()` convention).
     *
     * Ticks once per parked data slice reclaimed because @ref
     * transport_can_config_t::max_pending was reached or because it aged past
     * `rx_ttl` — a bounded, counted drop instead of the unbounded park this
     * replaces — and once per inbound slice whose bytes could not be owned
     * (@ref transport_can_config_t::rx_backend refused, #911). It counts SLICES,
     * not groups; a group's buffered slices reclaimed as a unit are @ref
     * dropped_groups, which the refusal also ticks because the group it belonged
     * to is abandoned rather than completed with a fabricated slice.
     */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept {
        return dropped_rx_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Reassembled groups dropped because they completed inside the
     *        sink-install window — the transport existed (link receiving, RX
     *        callback registered) but no receiver sink was installed yet
     *        (#1103, ADR-0081 §4).
     *
     * `transport_vertex_t::make_connection` constructs the link, then registers
     * the connection vertex, and only then wires the receiver via
     * `fwd_router_t::add_child`. A bus needs no provocation to fill that span:
     * any bystander traffic already on the wire lands in it. CAN is ADR-0081's
     * drop arm because neither escape exists — the bus has no per-peer flow
     * control to hold bytes in, and withholding the RX callback would starve the
     * liveness bookkeeping it drives (`last_heard`, the pending/reassembly
     * sweeps); parking the group inside the library is banned outright. So a
     * group that completes while both receiver slots are empty is dropped at the
     * delivery seam and counted HERE — a distinctly named cause, never folded
     * into @ref dropped_rx or @ref dropped_groups and never silent. Counts
     * GROUPS. A deployment that sees it moving is watching the sink-install
     * window, not guessing.
     */
    [[nodiscard]] std::uint64_t dropped_presink() const noexcept {
        return dropped_presink_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Inbound data slices refused because the binding they resolved to
     *        predates the producer's current endpoint-allocator lap — the WELD,
     *        counted (#1011).
     *
     * The retire-on-re-issue rule (#909) fires only on OVERLAP, so a binding whose
     * endpoint run a later advertise merely skipped over survives. Data slices whose
     * own advertise was lost on the bus then resolve first-match to that survivor,
     * fill the indices its lost slices left empty, and complete its stale group: two
     * unrelated payloads welded into one frame, trimmed to the length the STALE
     * manifest promised, and delivered upstream as valid. Silent, and the size the
     * receiver expected.
     *
     * The lap is what makes it decidable without spending endpoint bits (ADR-0077's
     * option 1, still declined): `alloc_base` issues strictly ascending bases and
     * wraps to @ref kCanFirstDataEndpoint, so an advertise whose base does not exceed
     * the last one seen from that node is proof the producer's allocator came round.
     * Every binding of that node then belongs to a PRIOR lap, and a slice resolving to
     * one is refused rather than welded: the group is discarded (ticking @ref
     * dropped_groups, as every other pre-delivery reclamation does) and the slice is
     * counted HERE.
     *
     * Counts SLICES, like @ref dropped_rx — one per refused slice, not one per group.
     * A distinctly named cause, never folded into @ref dropped_rx (which is
     * backpressure and age-out) and never silent: a deployment that sees this moving
     * is watching lost advertises on a lapping bus, not guessing.
     */
    [[nodiscard]] std::uint64_t dropped_stale_binding() const noexcept {
        return dropped_stale_binding_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Outbound frames the caller believed sent that never reached the bus
     *        (the twai `tx_dropped()` convention, spelled to match `dropped_rx`).
     *
     * Ticks when a `send` cannot own its bytes (allocation failure — the
     * backpressure case), when the payload splits into no window at all, when the
     * group needs more consecutive endpoint slots than @ref kCanMaxGroupSlices
     * (refused WHOLE, before any manifest goes out — #910), and when the group's
     * advertise manifest cannot be encoded.
     */
    [[nodiscard]] std::uint64_t dropped_tx() const noexcept {
        return dropped_tx_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Reassembly groups reclaimed before delivery — a `max_groups` eviction,
     *        an `rx_ttl` age-out, or a base-endpoint run being re-bound (#909).
     *
     * The third form is the wraparound one: the 12-bit endpoint space recurs, so a
     * fresh advertise over a run that a stale binding still claims retires that
     * binding and the group it was feeding. That group can never complete (nothing
     * resolves to it again) and its slices must not merge into the fresh group, so
     * it is reclaimed here rather than aged out — one counter for "a group's
     * buffered slices were reclaimed before delivery", whatever forced it.
     *
     * The accessor the reassembly buffer's own counter never had: it is private
     * state on the RX thread, so this reads it under the ingress lock.
     */
    [[nodiscard]] std::uint64_t dropped_groups() const;

    /** @brief Data slices currently parked awaiting their advertise (introspection). */
    [[nodiscard]] std::size_t pending_slices() const;

    /**
     * @brief The interface-level snapshot (#932) — what a generic `transport_t*` reads.
     *
     * `malformed_rx` stays zero: this bus has no framing-desync class to report — an
     * ingress slice is either owned and reassembled or shed into @ref dropped_rx — so
     * it reports nothing rather than folding a different meaning into that field.
     */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), 0, dropped_tx()};
    }

    // --- the bus capability (ADR-0044) ------------------------------------------

    /** @brief This link IS a bus — expose the @ref bus_link_t facet. */
    [[nodiscard]] bus_link_t* bus() override { return this; }

    /**
     * @brief Visit the peers currently audible on the bus, as `n<node-id>` names.
     *
     * Synthesized on the fly from the last-heard table — a snapshot of live
     * traffic, never stored graph structure (ADR-0044 §1). Entries older than the
     * configured `peer_ttl` are skipped.
     * @note @p visit runs under the table lock; it must not re-enter this link.
     */
    void enumerate_peers(const peer_visitor_t& visit) const override;

    /**
     * @brief Resolve `n<node-id>` to this bus's directed endpoint for that peer.
     * @retval nullptr @p peer is not a canonical peer name, or the peer expired.
     */
    [[nodiscard]] transport_t* peer_link(std::string_view peer) override;

    /** @brief True — the CAN bus reassembles into ropes and delivers them as-is. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

   private:
    // A directed per-peer sending endpoint (what peer_link returns): send() emits a
    // group whose advertise carries `target_node`, so only that peer delivers it.
    // Owned by its peer-table entry, whose map node never moves (insert-only table),
    // so a pointer handed out by peer_link stays valid for the transport's life.
    class peer_endpoint_t final : public transport_t {
       public:
        void send(std::span<const std::byte> frame) override;
        // Ingress is the owning transport's (its peer-named slot); the inherited
        // receiver slot of this directed facade is never delivered to.

        /** @brief The owning bus's snapshot (#932): a directed facade counts into the
         *         link's own counters, so it reports them rather than a fabricated zero. */
        [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
            return owner_ == nullptr ? transport_drop_stats_t{} : owner_->drop_stats();
        }

       private:
        friend class transport_can;
        transport_can* owner_ = nullptr;
        std::atomic<std::uint16_t> node_{0};
    };

    // One entry of the last-heard peer table (ADR-0044), keyed by node id. Entries
    // are INSERT-ONLY (unlike learned_, which retires overlapping runs — #909; this
    // table is keyed by node, and a node id never aliases): expiry hides an
    // entry from enumeration/resolution but never frees it, so the endpoint
    // facades peer_link hands out stay pointer-stable for the transport's life
    // (std::map nodes never move). Growth is one entry per DISTINCT node id ever
    // heard — structurally bounded by the 13-bit id space, never per-frame.
    struct peer_entry_t {
        std::chrono::steady_clock::time_point last_heard{};
        peer_endpoint_t endpoint;
    };

    // --- ingress (runs on the link's receive thread) ---
    void on_rx(const can_frame_data_t& frame);
    void learn_advertise(const can::advertise_t& adv);  // requires rx_m_ held
    // Retire every same-node binding whose endpoint run overlaps the one a fresh
    // advertise claims, and discard the reassembly group each was feeding (#909).
    // The 12-bit endpoint space wraps, so a base RECURS; without this a stale run
    // shadows the live binding in the process_data scan and a recurring key merges
    // stale slices into the fresh group. Holds the invariant "one binding per slot,
    // and a group lives exactly as long as the binding that feeds it".
    void invalidate_overlapping(const can::can_id_fields_t& base,
                                std::uint16_t slice_count);  // requires rx_m_ held
    // Mark every binding of `node` as belonging to a prior lap — the producer's
    // endpoint allocator has come round (#1011). Runs only on a detected lap, and
    // walks the same map invalidate_overlapping already walks per advertise.
    void mark_prior_lap(std::uint16_t node);           // requires rx_m_ held
    void process_data(const can_frame_data_t& frame);  // requires rx_m_ held
    void park_pending(const can_frame_data_t& frame);  // requires rx_m_ held
    void expire_pending();                             // requires rx_m_ held
    void deliver(std::uint16_t src_node, tr::view::rope_t frame);
    // Refresh/insert into the last-heard table, using the frame's arrival stamp.
    void touch_peer(std::uint16_t node, std::chrono::steady_clock::time_point now);

    // --- egress helpers ---
    // RESERVE the group's run of consecutive endpoint slots (#910). Fallible on
    // purpose: a group over kCanMaxGroupSlices fits at no base, and the caller must
    // learn that BEFORE it advertises a slice_count it cannot deliver.
    std::optional<std::uint16_t> alloc_base(std::size_t slice_count);  // requires tx_m_ held
    // Slices adv's 18-byte header and cfg_.path (in place, never copied — this node only
    // ever advertises its OWN path, so adv.path is left empty) into CLASSIC windows.
    void emit_advertise(const can::advertise_t& adv);  // requires tx_m_ held
    void send_impl(std::span<const std::byte> frame, std::uint16_t target);
    void emit_hello();  // the join-time presence advertise (slice_count == 0)

    std::unique_ptr<can_link_t> link_;
    transport_can_config_t cfg_;

    // egress
    std::mutex tx_m_;  // serializes whole-group emission
    std::uint16_t next_base_ = kCanFirstDataEndpoint;

    // ingress. A learned binding remembers whether its group is for THIS node —
    // a directed group addressed elsewhere is consumed but never reassembled.
    struct binding_t {
        can::advertise_t adv;
        bool deliver = true;
        // True once the producer's endpoint allocator has come round past this
        // binding: it was learned in a PRIOR lap, so no advertise of the current lap
        // stands behind it and a slice resolving to it would be welded (#1011).
        // Set by mark_prior_lap; never cleared — the fresh binding inserted after the
        // sweep starts false, and the only exit from a prior lap is being replaced.
        // Costs ZERO bytes: it lands in the tail padding `bool deliver` already had.
        bool prior_lap = false;
    };
    /**
     * @brief Per-remote-node receive state: its advertise byte stream, and the base
     *        endpoint of the last advertise it was seen to place.
     *
     * One map instead of two. The lap test needs a single `std::uint16_t` per node and
     * the control-stream buffer is already keyed by exactly that — a second
     * `std::map<std::uint16_t, ...>` would have cost a whole map node (allocator
     * header, three pointers, colour) to carry two bytes on a target where the RX
     * buffers are injected precisely because the heap is scarce.
     */
    struct node_rx_t {
        std::vector<std::byte> control; /**< @brief Accumulated advertise byte stream. */
        // The base endpoint of the most recent advertise learned from this node.
        // `alloc_base` issues strictly ascending bases and wraps to
        // kCanFirstDataEndpoint, so a fresh base that does not EXCEED this one is proof
        // the producer's allocator lapped. Starts at the control slot (0), which no
        // conforming data advertise can claim, so a node's first advertise never reads
        // as a lap.
        std::uint16_t last_base = kCanControlEndpoint;
    };
    /**
     * @brief One data slice parked until its advertise lands, with the stamp that
     *        lets it age out.
     *
     * A parked slice whose advertise never lands must not live forever, and the raw
     * frame alone carries no arrival time to decide that on.
     */
    struct pending_slice_t {
        can_frame_data_t frame{};                        /**< @brief The parked raw frame. */
        std::chrono::steady_clock::time_point arrived{}; /**< @brief When it was parked. */
    };

    std::mutex rx_m_;         // guards the map + buffers
    can_reassembly_t reasm_;  // data-slice reassembly
    // base CAN ID -> binding. NOT insert-only: a fresh advertise retires every
    // same-node entry whose endpoint run it overlaps (#909), which is what keeps a
    // recurring base from claiming two bindings at once. Growth is otherwise one
    // entry per distinct base a node advertises, structurally bounded by the 12-bit
    // endpoint space per node.
    std::map<std::uint32_t, binding_t> learned_;
    // node id -> its advertise byte stream + last advertised base (the lap witness).
    // Growth is one entry per distinct node heard, bounded by the 13-bit node space.
    std::map<std::uint16_t, node_rx_t> nodes_;
    // Data slices awaiting their advertise. Drawn from the injected resource (the
    // RX thread must not reach the global heap), bounded in COUNT by max_pending
    // and in AGE by rx_ttl; append-ordered, so both the stale prefix and the
    // evict-oldest end are the front.
    std::pmr::vector<pending_slice_t> pending_;
    std::chrono::steady_clock::time_point rx_now_{};  // current frame's arrival stamp
    // Where an inbound slice's bytes are copied (cfg_.rx_backend, resolved to the
    // process heap once in the constructor). Resolved rather than branched so the
    // per-slice path has one indirect call either way.
    mem::mem_backend_t* rx_backend_ = nullptr;

    // Drop counters (#912, #1103, #1011). Written on the RX/TX threads, read by anyone.
    std::atomic<std::uint64_t> dropped_rx_{0};
    std::atomic<std::uint64_t> dropped_tx_{0};
    std::atomic<std::uint64_t> dropped_presink_{0};
    std::atomic<std::uint64_t> dropped_stale_binding_{0};

    // the last-heard peer table (ADR-0044) — node id -> entry, insert-only
    mutable std::mutex peers_m_;
    std::map<std::uint16_t, peer_entry_t> peers_;

    // Inbound delivery goes through the inherited peer-named slot (bus_link_t::
    // peer_rx_) — the ONE tier-select mechanism; no transport-local receivers.
};

/**
 * @brief The ready-to-register `can` transport factory — how the CAN module plugs
 *        into the ADR-0027 connection-vertex catalog.
 *
 * Register at setup: `net.register_transport_type("can", can_transport_factory())`.
 * A subsequent `write /net:children[] += SPEC{type, name, config{kind = "can", …}}`
 * then constructs a @ref transport_can over a production @ref socketcan_link_t and
 * the connection vertex owns it. Per the ADR-0043 §5 leanness ruling, every
 * CAN-private key is parsed HERE from the raw config SETTINGS TLV — nothing lands
 * in the shared `conn_settings_t`:
 *  - `ifname` (NAME, required) — the SocketCAN interface (e.g. `"can0"`, `"vcan0"`);
 *  - `node` (VALUE u16, required) — this node's id in the structured 29-bit ID;
 *  - `version` (VALUE u8) — the protocol-version prefix (default 0);
 *  - `fd` (VALUE u8) — non-zero selects CAN-FD framing (default classic);
 *  - `path` (NAME) — the identity path advertised for this node's groups;
 *  - `peer_ttl_ms` (VALUE u32) — the ADR-0044 peer liveness window;
 *  - `max_groups` (VALUE u32) — the live reassembly-group ceiling (0 = unbounded,
 *    host-bounded per RFC-0006); this is what makes the reassembly buffer's
 *    evict-oldest seam reachable in production at all (#912);
 *  - `max_pending` (VALUE u32) — the ceiling on data slices parked awaiting their
 *    advertise (0 = unbounded, host-bounded per RFC-0006);
 *  - `rx_ttl_ms` (VALUE u32) — the RX staleness window (0 = track `peer_ttl_ms`).
 * A missing/invalid `ifname` or `node` fails with `TYPE_MISMATCH`; a socket that
 * cannot bind (no kernel CAN / non-Linux stub) fails with `TRANSPORT_DOWN` — the
 * TRANSIENT status, because the address resolved and it was the link that did not
 * come up (#929). The universal `role` is ignored — a bus has no dial/listen
 * asymmetry.
 *
 * @param reasm_mr Where every constructed transport's RX buffers draw their
 *                 structure — the injection point for the pmr seam the config TLV
 *                 cannot carry (a resource is a pointer, not a wire value). A host
 *                 registers the factory with its own bounded resource; the default
 *                 is the process heap. Must outlive every transport built here.
 * @param rx_backend Where every constructed transport COPIES an inbound data slice's
 *                 bytes (@ref transport_can_config_t::rx_backend). Injected here for
 *                 the same reason as @p reasm_mr — a backend is a pointer, not a wire
 *                 value — so the seam is reachable from production registration and
 *                 not only from a unit test. `nullptr` = the process heap. Must
 *                 outlive every transport built here.
 * @return The factory functor for @ref transport_vertex_t::register_transport_type.
 */
[[nodiscard]] transport_vertex_t::transport_factory_t can_transport_factory(
    std::pmr::memory_resource* reasm_mr = std::pmr::new_delete_resource(),
    mem::mem_backend_t* rx_backend = nullptr);

}  // namespace tr::net
