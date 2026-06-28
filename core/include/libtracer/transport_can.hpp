/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_can (increment 2 of #55) — the SocketCAN binding that drives the
 * pure framing layer (can.hpp / view_can.hpp / mem_can_reassembly.hpp) over a
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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/can.hpp"
#include "libtracer/mem_can_reassembly.hpp"
#include "libtracer/transport.hpp"
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
    std::array<std::byte, 64>
        data{}; /**< @brief The data field; only the first @ref len bytes are live. */

    /** @brief The live data-field bytes as a read-only span. */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>(data.data(), len);
    }
};

/**
 * @brief The raw-frame seam between @ref transport_can and a physical CAN bus.
 *
 * Abstracting the socket here is what makes @ref transport_can testable without
 * the kernel `vcan` module: production uses @ref socketcan_link_t, tests use an
 * in-memory paired link. A link is single-owner (held by one transport) and
 * delivers inbound frames through the registered @ref rx_fn_t, which may fire on
 * an internal receive thread.
 */
class can_link_t {
   public:
    /** @brief Callback invoked once per inbound raw CAN frame (may run off-thread). */
    using rx_fn_t = std::function<void(const can_frame_data_t&)>;

    virtual ~can_link_t() = default;

    /** @brief Emit one raw CAN frame onto the bus. */
    virtual void write_raw(const can_frame_data_t& frame) = 0;

    /** @brief Register the sink for inbound raw frames; set before frames flow. */
    virtual void on_receive(rx_fn_t rx) = 0;
};

/**
 * @brief The production `can_link_t` — a real Linux SocketCAN `PF_CAN` socket.
 *
 * Opens a `socket(PF_CAN, SOCK_RAW, CAN_RAW)`, enables CAN-FD frames
 * (`CAN_RAW_FD_FRAMES`, best-effort — a classic-only controller still works),
 * binds to the named interface (e.g. `"vcan0"`/`"can0"`), and spawns a receive
 * thread that translates each kernel frame into a @ref can_frame_data_t for the
 * registered callback. Compiled only on Linux (`<linux/can.h>`); on other
 * platforms it is a stub whose @ref ok is always false, so sanitizer/non-Linux
 * builds stay clean. The send path is `MSG_NOSIGNAL`-equivalent (a raw CAN write
 * cannot SIGPIPE) and serialized; the fd is reset under the write lock before
 * close on shutdown.
 */
class socketcan_link_t : public can_link_t {
   public:
    /**
     * @brief Open + bind a `CAN_RAW` socket on interface @p ifname.
     * @param ifname The CAN network interface name (e.g. `"vcan0"`).
     */
    explicit socketcan_link_t(const std::string& ifname);

    /** @brief Stop the receive thread and close the socket. */
    ~socketcan_link_t() override;

    socketcan_link_t(const socketcan_link_t&) = delete;
    socketcan_link_t& operator=(const socketcan_link_t&) = delete;

    /** @brief Write one frame to the bus (classic or FD per @ref can_frame_data_t::fd). */
    void write_raw(const can_frame_data_t& frame) override;

    /** @brief Register the inbound-frame sink (invoked on the receive thread). */
    void on_receive(rx_fn_t rx) override;

    /** @brief True if the socket opened and bound (false on non-Linux or any error). */
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

   private:
    void run();  // receive thread

    int fd_ = -1;
    rx_fn_t rx_;          // guarded by m_
    std::mutex m_;        // guards rx_
    std::mutex write_m_;  // serializes writes / fd teardown
    std::atomic<bool> stop_{false};
    std::thread thread_;
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
};

/**
 * @brief A `transport_t` over Linux SocketCAN — header-elided, self-establishing.
 *
 * Wires the increment-1 framing to a live bus. **Egress** (@ref send): the frame
 * is address-shift-fragmented by @ref tr::view::view_can_frames_t into CAN data
 * fields, an in-band @ref tr::net::can::advertise_t manifest (carrying the slice
 * count and exact total length) is emitted on the control ID, then the lean
 * id-matched data frames follow — CAN-FD windows DLC-padded up to a legal size.
 * **Ingress** (the link's receive thread): advertise frames populate the dynamic
 * identity↔path map; data frames are reassembled by @ref
 * tr::mem::mem_can_reassembly_t keyed by `(node, base-endpoint) + slice-index`,
 * trimmed back to the advertised total (undoing FD padding), and delivered
 * byte-exact to the receiver. The map is rebuilt purely from advertise frames, so
 * a rejoining node self-heals with no coordinator (ADR-0030).
 */
class transport_can : public transport_t {
   public:
    /**
     * @brief Bind this transport to raw link @p link with node identity @p config.
     * @param link   The owned raw-frame link (a @ref socketcan_link_t in production).
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
     * @brief Register the sink for reassembled inbound frames.
     * @param receiver Callback invoked on the link's receive thread with byte-exact frames.
     */
    void set_receiver(receiver_t receiver) override;

    /**
     * @brief Look up a learned `id ↔ path` binding by its base CAN ID (test/introspection hook).
     * @param base_can_id The advertised group's base 29-bit CAN ID.
     * @return The learned @ref tr::net::can::advertise_t, or `std::nullopt` if unknown.
     */
    [[nodiscard]] std::optional<can::advertise_t> learned_binding(std::uint32_t base_can_id) const;

   private:
    // --- ingress (runs on the link's receive thread) ---
    void on_rx(const can_frame_data_t& frame);
    void learn_advertise(const can::advertise_t& adv);  // requires rx_m_ held
    void process_data(const can_frame_data_t& frame);   // requires rx_m_ held
    void deliver(std::span<const std::byte> frame);

    // --- egress helpers (run under tx_m_) ---
    std::uint16_t alloc_base(std::size_t slice_count);  // requires tx_m_ held
    void emit_advertise(const can::advertise_t& adv);   // requires tx_m_ held

    std::unique_ptr<can_link_t> link_;
    transport_can_config_t cfg_;

    // egress
    std::mutex tx_m_;  // serializes whole-group emission
    std::uint16_t next_base_ = kCanFirstDataEndpoint;

    // ingress
    std::mutex rx_m_;                                          // guards the map + buffers
    tr::mem::mem_can_reassembly_t reasm_;                      // data-slice reassembly
    std::map<std::uint32_t, can::advertise_t> learned_;        // base CAN ID -> binding
    std::map<std::uint16_t, std::vector<std::byte>> control_;  // per-node advertise byte stream
    std::vector<can_frame_data_t> pending_;  // data frames awaiting their advertise

    // receiver
    std::mutex m_;
    receiver_t receiver_;
};

}  // namespace tr::net
