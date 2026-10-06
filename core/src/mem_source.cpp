/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_source.hpp"

#include <cstdlib>

#include "libtracer/init_fault.hpp"

/**
 * @file
 * @brief The process-wide default @ref tr::mem::block_source_t.
 */

namespace tr::mem {
namespace {

/**
 * @brief The process-wide default source.
 *
 * Namespace-scope `constinit`, NOT a function-local static: the latter (as in
 * @ref heap_backend, `mem_heap.cpp`) costs a `__cxa_guard` word in `.bss` and an
 * acquire fence on EVERY call. This one is on the registration path, so the
 * precedent is deliberately not copied.
 */
constinit heap_source_t g_heap_source{};

/** @brief The process-wide null source (same constant-init discipline). */
constinit null_source_t g_null_source{};

}  // namespace

block_source_t& heap_source() noexcept { return g_heap_source; }

block_source_t& null_source() noexcept { return g_null_source; }

void exhausted_at_init(const block_source_t& src, const char* what) noexcept {
    const source_stats_t s = src.stats();
    // Reported through the build's sink, not written here: core owns no output stream (#1885).
    graph::config_t::fault_sink_t::report(init_fault_t{.kind = init_fault_kind_t::SOURCE_EXHAUSTED,
                                                       .call = what,
                                                       .source = src.name(),
                                                       .needed = s.largest_refused,
                                                       .in_use = s.in_use});
    std::abort();
}

}  // namespace tr::mem
