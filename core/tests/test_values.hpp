/**
 * @file
 * @brief The one owned-bytes `view_t` builder the test tree shares (#874).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `make_value` — heap-allocate a segment, copy bytes into it, hand back a `view_t` over it —
 * was defined **32 times across 25 test files**, in five spellings that differed only in
 * whether they named `tr::view::view_t` or an imported `view_t`, and in whether the
 * `initializer_list` overload went through a `std::vector` or filled the segment directly.
 *
 * @par Why not `tr::view::over_bytes`
 * Production has a near-twin (`%mem_heap.hpp`), and it is deliberately NOT what the tests
 * want: `over_bytes` answers `std::optional<view_t>` so a caller can map an allocation failure
 * to BACKPRESSURE, and it answers an **engaged-but-unowned** view for an empty input rather
 * than allocating. A test asserting "this vertex holds a zero-length value" needs the owned
 * empty segment, and a test asserting a write succeeded wants the `view_t` itself, not an
 * optional it must unwrap. Keeping the helper test-side also keeps the tests honest: they
 * exercise the production seam under test, not the helper that happens to sit beside it.
 *
 * Split from @ref test_support.hpp because this header needs a libtracer type and that one
 * needs nothing but the standard library — `substrate_test_no_atomic` and
 * `pool_only_dispatch_test` compile a restricted source set and include only the runner.
 *
 * `tr::testing` is a tests-only namespace — it is not a layer in the L0..L5 model and nothing
 * under `core/src` or `core/include` may name it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/view.hpp"

namespace tr::testing {

/**
 * @brief A `view_t` over a fresh, owned heap segment holding @p bytes.
 *
 * An empty @p bytes still allocates: the result is an OWNED zero-length segment, which is what
 * "the vertex holds an empty value" means on the wire, and is distinguishable from a default
 * `view_t` that owns nothing.
 */
inline tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief @ref make_value from a braced byte list — `make_value({0x01, 0x02})`. */
inline tr::view::view_t make_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> v;
    v.reserve(bytes.size());
    for (const std::uint8_t b : bytes) v.push_back(std::byte{b});
    return make_value(v);
}

}  // namespace tr::testing
