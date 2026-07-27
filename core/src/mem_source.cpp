/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_source.hpp"

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

}  // namespace tr::mem
