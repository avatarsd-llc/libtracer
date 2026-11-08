/**
 * @file
 * @brief The Cortex-M0 footprint gate's CAN row (#2401): the portable half of the CAN
 *        transport, the wire framing (`can_framing.hpp`) and the multi-frame reassembly
 *        buffer (`can_reassembly.hpp`), linked on top of the required-modules node.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `can_transport_t` itself does not build on this target: it is a vertex of the graph runtime
 * and the net plane, which need an RTOS binding for `config_t::sync_t`, and it runs its
 * receive loop on a pthread. A bare-metal Cortex-M0 has neither. What a NARROW CAN node can
 * carry from libtracer is the framing and the reassembly, and this fixture measures exactly
 * that. tools/cortexm0_footprint.py links it with the sentinel node and keeps
 * `lt_footprint_row` as a root (`-Wl,-u`), so the row is the flash this code adds to the node.
 *
 * Keep it stable, like sentinel_node.cpp: the row is pinned, so a change here moves the pin.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "libtracer/can_framing.hpp"
#include "libtracer/can_reassembly.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/view.hpp"

namespace {

/** @brief The receive store of record on an MCU (ADR-0016): a caller-owned static slab. */
alignas(std::max_align_t) std::array<std::byte, 2048> g_row_slab{};

/** @brief A payload the optimizer cannot see through, so the framing is really linked. */
volatile std::uint8_t g_seed = 0x5a;

}  // namespace

/**
 * @brief The row's workload: slice one payload into classic and FD frames, then reassemble
 *        the classic slices out of order into one segment and fold what comes back.
 * @return A fold of the reassembled bytes, so nothing above is dead code.
 */
extern "C" std::uint32_t lt_footprint_row() {
    using namespace tr;
    mem::pool_t pool(g_row_slab, 128);
    std::uint32_t acc = 0;

    view::segment_t* const seg = pool.alloc(40);
    if (seg == nullptr) return 0;
    view::segment_ptr_t owner = view::segment_ptr_t::adopt(seg);
    for (std::size_t i = 0; i < 40; ++i)
        owner->bytes[i] = static_cast<std::byte>(g_seed + static_cast<std::uint8_t>(i));
    const view::view_t payload = view::view_t::over(owner);

    // Framing: the windows a sender puts on the bus, classic and FD.
    for (const auto mode : {net::can::can_frame_mode_t::CLASSIC, net::can::can_frame_mode_t::FD})
        acc += static_cast<std::uint32_t>(net::can::can_frame_count(payload, mode));

    // Reassembly: admit the group by its advertise, add the slices in reverse, take it whole.
    net::can_reassembly_t rx(pool, 4, 256, 512);
    const net::reassembly_key_t key{.origin = {1, 2, 3}, .ts = g_seed};
    const auto mode = net::can::can_frame_mode_t::CLASSIC;
    const std::size_t count = net::can::can_frame_count(payload, mode);
    if (rx.open_group(key, static_cast<std::uint32_t>(count), 40,
                      net::can::can_max_data(net::can::can_frame_mode_t::FD), 64, pool)) {
        for (std::size_t i = count; i-- > 0;) {
            const view::view_t w = net::can::can_frame_at(payload, mode, i);
            (void)rx.add_slice(key, static_cast<std::uint32_t>(i), w.bytes());
        }
        if (rx.is_complete(key)) {
            if (const auto whole = rx.take(key)) {
                for (const std::byte b : whole->bytes())
                    acc = acc * 131u + std::to_integer<std::uint8_t>(b);
            }
        }
    }
    rx.set_now(g_seed);
    acc += static_cast<std::uint32_t>(rx.sweep_stale(1) + rx.dropped_groups());
    return acc;
}
