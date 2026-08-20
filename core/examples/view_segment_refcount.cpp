/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a segment is refcounted, and copying a view is a refcount bump.
 *
 * The L0↔L1 boundary object is a **segment**: real bytes, the backend that reclaims them,
 * and an intrusive refcount (`docs/reference/08-views-and-ownership.md`). A **view** is a
 * `{owner, offset, length}` window over one segment, and it holds a `segment_ptr_t` — so
 * copying a view CLONES the reference and the bytes stay alive as long as any view names
 * them. That is the whole safety story behind zero-copy fan-out: a decoded TLV's spans stay
 * valid because the view that produced them is still holding its segment.
 *
 * Reclaim is observed here through a `tr::mem::pool_t` over a stack slab, whose free-slot
 * count moves — the heap backend would reclaim just as correctly, but invisibly.
 *
 * Runs under ctest as `example_view_segment_refcount`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>

#include "libtracer/tracer.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    bool ok = true;
    alignas(std::max_align_t) std::array<std::byte, 1024> slab{};
    tr::mem::pool_t pool{slab, 64};

    tr::view::segment_ptr_t seg = tr::view::segment_alloc(pool, 8);
    check(ok, static_cast<bool>(seg), "the pool handed out a segment");
    if (!seg) return 1;
    std::printf("fresh segment: use_count=%u, pool has %zu/%zu slots free\n", seg.use_count(),
                pool.available(), pool.capacity());
    check(ok, seg.use_count() == 1, "a fresh segment starts at one reference");

    {
        // over() takes the handle by value, so passing a COPY leaves `seg` holding its own.
        tr::view::view_t v = tr::view::view_t::over(seg);
        check(ok, seg.use_count() == 2, "a view over it holds a second reference");

        tr::view::view_t clone = v;  // copy == clone: a refcount bump, never a byte copy
        check(ok, seg.use_count() == 3, "copying the view clones the reference, not the bytes");
        check(ok, clone.bytes().data() == v.bytes().data(), "both windows address the same bytes");
        std::printf("two views later: use_count=%u\n", seg.use_count());
    }
    check(ok, seg.use_count() == 1, "the views went out of scope and released");
    check(ok, pool.available() == pool.capacity() - 1, "the slot is still held — one ref remains");

    seg.reset();  // the LAST reference: this is what fires the backend's destroy
    std::printf("after the last drop: pool has %zu/%zu slots free\n", pool.available(),
                pool.capacity());
    check(ok, pool.available() == pool.capacity(), "the bytes are reclaimed at the last drop");
    return ok ? 0 : 1;
}
