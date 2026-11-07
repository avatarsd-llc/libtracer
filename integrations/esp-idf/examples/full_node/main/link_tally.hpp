/**
 * @file
 * @brief full_node's frame observer: the router's `config_t::frame_observer_t` bound through
 *        the component's `LIBTRACER_FRAME_OBSERVER_*` variables (#2595).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The project CMakeLists.txt names this header and `full_node::link_tally_t` before
 * `project()`, and the component's generated `libtracer/config_override.hpp` includes it and
 * binds the type. So it is compiled into the component's router as well as into this app, and
 * it includes only the standard library: a libtracer header here would include `config.hpp`,
 * which is what includes this file.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace full_node {

/** @brief Counts the inbound frames every router in the image observed, on any link. */
struct link_tally_t {
    /** @brief Inbound frames observed so far; the self-proof reads it. */
    static inline std::atomic<std::uint32_t> frames{0};

    /** @brief Counts one inbound frame; the link name is not kept. */
    static void on_frame(std::string_view /*inbound_link_name*/) noexcept {
        frames.fetch_add(1, std::memory_order_relaxed);
    }
};

}  // namespace full_node
