/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `decode_into`: the same frame as a flat arena, drawn from a stack slab.
 *
 * `decode` returns an OWNING `tlv_t` whose `children` vectors allocate on the global heap by
 * construction. `wire::decode_into` (ADR-0041) answers the same grammar with a flat,
 * pre-order `arena_tlv_t` array whose storage comes from an injected
 * `tr::mem::block_source_t` — point that at a `bump_source_t` over a stack buffer and the
 * whole decode touches no heap. Every span in the arena borrows the input buffer; the arena
 * holds structure only, never bytes.
 *
 * Navigation is index arithmetic rather than pointer chasing: children of node `i` start at
 * `i + 1`, and `end` is one past the last descendant, so `next_sibling` is a single load.
 *
 * Runs under ctest as `example_wire_arena_decode`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::wire::tlv_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    bool ok = true;
    const std::vector<std::byte> x{std::byte{0x11}, std::byte{0x22}};
    const std::vector<std::byte> y{std::byte{0x33}, std::byte{0x44}};

    tlv_t point;
    point.type = type_t::POINT;
    point.opt.pl = true;
    point.children.push_back(tlv_t{.type = type_t::VALUE, .payload = std::span(x)});
    point.children.push_back(tlv_t{.type = type_t::VALUE, .payload = std::span(y)});
    const std::vector<std::byte> frame = tr::wire::encode(point);

    // The whole decode's storage: one stack buffer. null_source() upstream makes the buffer
    // the HARD bound — an overflowing frame is refused, never quietly served from the heap.
    std::array<std::byte, 1024> slab{};
    tr::mem::bump_source_t bump{slab, tr::mem::null_source()};

    const auto arena = tr::wire::decode_into(frame, bump);
    check(ok, arena.has_value(), "the frame decodes into the caller's slab");
    if (!arena) return 1;

    std::printf("%zu nodes, %zu slab bytes used, zero heap allocations\n", arena->size(),
                bump.used());
    check(ok, arena->size() == 3, "root plus two children, flat and pre-order");
    check(ok, arena->root().type == type_t::POINT, "index 0 is the root");

    const std::uint32_t first = tr::wire::tlv_arena_t::first_child(0);
    check(ok, first == 1 && (*arena)[first].type == type_t::VALUE, "a child starts at i + 1");
    const std::uint32_t second = arena->next_sibling(first);
    check(ok, second == 2 && (*arena)[second].type == type_t::VALUE,
          "and its sibling is one load away — next_sibling IS the subtree end");
    check(ok, arena->next_sibling(second) == arena->root().end,
          "walking off the last child lands exactly on the parent's end");
    const std::span<const std::byte> body = (*arena)[second].body;
    check(ok, body.data() == frame.data() + (frame.size() - body.size()),
          "and the node's body is a span INTO the input buffer — no bytes were copied");
    return ok ? 0 : 1;
}
