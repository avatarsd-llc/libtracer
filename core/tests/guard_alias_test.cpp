/**
 * @file
 * @brief The one-release alias window of #1703: every pre-move spelling of the guard vocabulary
 *        still names the layer-neutral type it moved to.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Compile-time only. When the aliases are removed, this file goes with them.
 */
#include <type_traits>

#include "libtracer/mem_pool.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/reader_guard.hpp"
#include "libtracer/vertex.hpp"

static_assert(std::is_same_v<tr::graph::mutex_guard_t, tr::mutex_guard_t>);
static_assert(std::is_same_v<tr::graph::no_guard_t, tr::no_guard_t>);
static_assert(
    std::is_same_v<tr::graph::guard_scope_t<tr::no_guard_t>, tr::guard_scope_t<tr::no_guard_t>>);
static_assert(std::is_same_v<tr::graph::reader_guard_t, tr::graph::guard_t>);
static_assert(std::is_same_v<tr::graph::guard_t, tr::graph::config_t::guard_t>);
static_assert(tr::graph::reader_guard<tr::mutex_guard_t> && tr::guard<tr::mutex_guard_t>);
static_assert(tr::graph::reader_guard<tr::no_guard_t> && tr::guard<tr::no_guard_t>);
static_assert(std::is_same_v<tr::mem::sync_none_t, tr::no_guard_t>);
static_assert(std::is_empty_v<tr::no_guard_t>, "[[no_unique_address]] must erase the no-op guard");
static_assert(tr::lockable<tr::no_guard_t> && !tr::guard<int>);

/** @brief Old spellings in a template argument still instantiate the moved templates. */
using old_spelling_pool_t = tr::mem::synchronized_pool_t<tr::graph::reader_guard_t>;
static_assert(std::is_same_v<old_spelling_pool_t, tr::mem::synchronized_pool_t<>>);
static_assert(
    std::is_same_v<tr::mem::pool_source_t<tr::mem::sync_none_t>, tr::mem::pool_source_t<>>);

/** @brief Nothing to run: the assertions above are the test. */
int main() { return 0; }
