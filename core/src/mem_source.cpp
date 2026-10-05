/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_source.hpp"

#include <cstdio>
#include <cstdlib>

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
    std::fprintf(stderr,
                 "libtracer: %s: the \"%s\" memory source refused an allocation at "
                 "initialization (%zu bytes needed, %zu bytes in use): a sizing bug, give it "
                 "more room (ADR-0056, ADR-0083)\n",
                 what, src.name(), s.largest_refused, s.in_use);
    std::abort();
}

}  // namespace tr::mem
