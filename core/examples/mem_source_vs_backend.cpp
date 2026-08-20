/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — L0 has TWO seams, and the number of owners picks between them.
 *
 * `tr::mem::mem_backend_t` vends a refcounted `tr::view::segment_t`: real bytes plus a
 * control block plus an intrusive count, so many views can share one buffer and the last
 * drop reclaims it. That is the right shape for PAYLOAD — a decoded frame's spans, a value
 * published to several subscribers.
 *
 * `tr::mem::block_source_t` vends raw bytes with a single owner and no header at all. That
 * is the right shape for the objects a node builds when it REGISTERS something — a vertex,
 * a route label, a reassembly entry — because a refcount on a thing with one owner is pure
 * overhead (`docs/reference/09-memory-substrate.md` §the second L0 seam).
 *
 * The two are injected independently. A node may point both at the same slab ("one slab,
 * whole stack") or split them; what it must not do is reach for a refcount it will never
 * share, or share bytes that carry no count.
 *
 * Runs under ctest as `example_mem_source_vs_backend`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/segment.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    bool ok = true;

    // Seam 1 — the BACKEND: bytes that will be shared. The handle is the ownership.
    tr::view::segment_ptr_t payload = tr::view::segment_alloc(tr::mem::heap_backend(), 64);
    check(ok, static_cast<bool>(payload), "the backend vends a refcounted segment");
    check(ok, payload.use_count() == 1, "one handle, one reference");
    {
        const tr::view::segment_ptr_t shared = payload;  // copy == clone, not a byte copy
        check(ok, payload.use_count() == 2, "a second holder is a count, never a second buffer");
        check(ok, shared->bytes.data() == payload->bytes.data(), "both name the SAME bytes");
    }
    check(ok, payload.use_count() == 1, "and the reclaim waits for the LAST holder");
    std::printf("segment: %zu shared bytes behind a %zu-byte control block\n",
                payload->bytes.size(), sizeof(tr::view::segment_t));

    // Seam 2 — the SOURCE: bytes with exactly one owner. No handle, no count, no header.
    alignas(std::max_align_t) std::array<std::byte, 64> slab{};
    std::array<tr::mem::size_class_t, 2> classes{};
    tr::mem::pool_source_t<> source{slab, classes};
    void* const object = source.try_alloc(64, 8);
    check(ok, object != nullptr, "the source vends 64 raw bytes out of a 64-byte slab");
    check(ok, source.used() == 64,
          "exactly 64 — a source block carries NO header, so nothing was left over");
    check(ok, source.try_alloc(8, 8) == nullptr, "the slab is the bound, and it is full");
    source.release(object, 64, 8);  // the owner returns it explicitly; there is no count to fall

    check(ok, source.try_alloc(64, 8) == object,
          "one owner means reclaim is a call, not a consequence of the last drop");
    return ok ? 0 : 1;
}
