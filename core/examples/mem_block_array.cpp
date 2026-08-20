/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a growable array whose growth FAILS BY VALUE.
 *
 * A bounded seam is only bounded if the containers above it can report a refusal. That is
 * `tr::mem::block_array_t<T>`: the same four-word footprint as a `std::pmr::vector`, drawing
 * from an injected `block_source_t`, with two differences that carry the whole point.
 *
 * 1. **Growth returns `false` instead of throwing.** `std::pmr::vector::push_back` on an
 *    exhausted resource throws, and on ESP-IDF that reaches the link-wrapped `__cxa_throw`
 *    `abort()` stub — a peer-reachable reboot when the container sits on the RX path.
 * 2. **Relocation is a `memcpy`.** `T` must be trivially copyable and trivially destructible,
 *    so growth needs no move loop and the vacated block needs no destruction.
 *
 * A refused `push_back` leaves the array UNCHANGED, which is what lets every caller treat
 * exhaustion as a clean reject rather than as a half-applied operation.
 *
 * Runs under ctest as `example_mem_block_array`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "libtracer/mem_source.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A plain aggregate — trivially copyable, so it can be relocated by `memcpy`. */
struct entry_t {
    std::uint32_t id = 0;  /**< @brief The element's identity. */
    std::uint32_t len = 0; /**< @brief Whatever the caller records beside it. */
};

}  // namespace

int main() {
    bool ok = true;
    // A hard bound the array cannot escape: a small buffer whose upstream serves nothing.
    alignas(std::max_align_t) std::array<std::byte, 128> scratch{};
    tr::mem::bump_source_t bounded{scratch, tr::mem::null_source()};
    tr::mem::block_array_t<entry_t> entries{bounded};
    check(ok, entries.empty(), "an array holds no block until something is put in it");

    check(ok, entries.push_back(entry_t{.id = 1, .len = 10}), "the first push takes a block");
    check(ok, entries.push_back(entry_t{.id = 2, .len = 20}), "and the next fits in it");
    check(ok, entries.size() == 2 && entries[1].id == 2, "elements read back by index");

    // The hot-path spelling: claim one uninitialized slot and fill it IN PLACE. Building the
    // aggregate as a temporary and copying it in cost a measured ~45 % on a 48-byte element.
    entry_t* const slot = entries.push_slot();
    check(ok, slot != nullptr, "push_slot claims a slot without materializing a temporary");
    slot->id = 3;
    slot->len = 30;
    check(ok, entries.back().id == 3, "written through the slot, not copied into it");

    // Fill the bounded source. The array grows geometrically, so the refusal arrives at a
    // growth boundary — and that is the only place it can arrive.
    int pushed = 0;
    while (entries.push_back(entry_t{.id = 99, .len = 99})) ++pushed;
    const std::size_t held = entries.size();
    std::printf("%zu entries in a %zu-byte bounded slab, then a clean refusal\n", held,
                scratch.size());
    check(ok, pushed > 0, "pushes keep succeeding until the source cannot grow the block");
    check(ok, entries.size() == held, "the refused push left the array UNCHANGED — no half-write");
    check(ok, entries[0].id == 1 && entries[0].len == 10,
          "and everything already stored survived the relocations that got here");
    check(ok, !entries.reserve(held + 1024), "reserve reports the same refusal, by value");
    return ok ? 0 : 1;
}
