/**
 * @file
 * @brief The setup-time sizing fault and the sinks it is reported through (#1885).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Core has exactly one fatal it reports before it aborts: a setup-time call that a bound refused
 * — a memory source too small for what initialization needs, or the vertex ceiling (ADR-0056
 * amendment, ADR-0083). Core does not write that report anywhere itself. It hands the facts to
 * the build's fault sink, `tr::graph::config_t::fault_sink_t`, and aborts after the sink returns.
 *
 * The sink is a compile-time binding, like `guard_t`: the host default writes one line to
 * `stderr` (@ref stderr_fault_sink_t), and a target without stdio binds @ref silent_fault_sink_t
 * or its own type (a UART write, a crash-log record) in `libtracer/config_override.hpp`.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace tr {

/** @brief Which bound refused a setup-time call. */
enum class init_fault_kind_t : std::uint8_t {
    SOURCE_EXHAUSTED, /**< @brief A memory source refused an allocation. */
    VERTEX_CEILING,   /**< @brief The graph's vertex ceiling refused a registration. */
};

/**
 * @brief The facts of one setup-time sizing fault — what a sink reports before core aborts.
 *
 * Plain data, so a sink on a target without a formatter can store or transmit it as it is.
 */
struct init_fault_t {
    init_fault_kind_t kind = init_fault_kind_t::SOURCE_EXHAUSTED; /**< @brief Which bound. */
    const char* call = "";   /**< @brief The refused call, e.g. `"register_vertex"`. */
    const char* source = ""; /**< @brief The refusing source's name; empty for the ceiling. */
    std::size_t needed = 0;  /**< @brief Bytes the refused request asked for. */
    std::size_t in_use = 0;  /**< @brief Bytes the source had in use when it refused. */
    std::size_t ceiling = 0; /**< @brief The vertex ceiling, for `VERTEX_CEILING`. */
};

/**
 * @brief The host default sink: one line on `stderr`, naming the call, the bound and the size.
 *
 * The line is the one core printed itself before #1885, byte for byte.
 */
struct stderr_fault_sink_t {
    /** @brief Write @p f to `stderr`. */
    static void report(const init_fault_t& f) noexcept {
        if (f.kind == init_fault_kind_t::VERTEX_CEILING) {
            std::fprintf(stderr,
                         "libtracer: %s: the vertex ceiling (%zu vertices) refused a "
                         "registration at initialization: a sizing bug, raise set_vertex_ceiling "
                         "(ADR-0056)\n",
                         f.call, f.ceiling);
            return;
        }
        std::fprintf(stderr,
                     "libtracer: %s: the \"%s\" memory source refused an allocation at "
                     "initialization (%zu bytes needed, %zu bytes in use): a sizing bug, give it "
                     "more room (ADR-0056, ADR-0083)\n",
                     f.call, f.source, f.needed, f.in_use);
    }
};

/** @brief A sink that reports nothing — for a target with no output; core still aborts. */
struct silent_fault_sink_t {
    /** @brief Discard @p f. */
    static void report(const init_fault_t& f) noexcept { (void)f; }
};

}  // namespace tr
