/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The build-time-closed module-set destroy dispatch (ADR-0047 §2). This is the
 * one TU that includes every fast-set backend's concrete type, so
 * segment_ptr_t::reset (segment.hpp) can reclaim a segment by its backend_tag —
 * a switch to a devirtualized direct call — while keeping the L0 seam
 * (segment.hpp / backend.hpp) free of any dependency on the concrete backends.
 *
 * Every fast case dispatches through the backend's `final`-class type with a
 * qualified call (a non-virtual, inlinable direct call). Any other tag (UNKNOWN,
 * CUDA, or a future backend not linked into the fast set) falls through to the
 * backend's virtual `destroy`, so the result is identical to the pre-ADR-0047
 * `seg->backend->destroy(seg)` for every backend — the tag is a fast path, never
 * a correctness dependency.
 */
#include "libtracer/backend.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/segment.hpp"

#ifndef LIBTRACER_BACKEND_SET_POOL_ONLY
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_heap.hpp"
#endif

namespace tr::mem {

#ifdef LIBTRACER_BACKEND_SET_POOL_ONLY

// Single-member module set (the MCU profile): the target links only mem_pool, so
// the dispatch folds to one direct call — no switch, no other backends' destroy
// code pulled in. This is the ADR-0047 §2 fold: on a single-backend target the
// seam compiles to nothing more than the direct release. `-D` this per target.
void destroy_dispatch(view::segment_t* seg) noexcept {
    static_cast<pool_t*>(seg->backend)->pool_t::destroy(seg);
}

#else

using detail::borrowed_backend_t;
using detail::borrowed_device_backend_t;

// The multi-member host module set: a switch to a devirtualized (`final`-class,
// qualified) direct call per linked backend, with the virtual `destroy` as the
// fallback for any tag not in the fast set.
void destroy_dispatch(view::segment_t* seg) noexcept {
    switch (seg->btag) {
        case backend_tag::HEAP:
            static_cast<heap_backend_t*>(seg->backend)->heap_backend_t::destroy(seg);
            return;
        case backend_tag::POOL:
            static_cast<pool_t*>(seg->backend)->pool_t::destroy(seg);
            return;
        case backend_tag::BORROWED:
            static_cast<borrowed_backend_t*>(seg->backend)->borrowed_backend_t::destroy(seg);
            return;
        case backend_tag::BORROWED_DEVICE:
            static_cast<borrowed_device_backend_t*>(seg->backend)
                ->borrowed_device_backend_t::destroy(seg);
            return;
        case backend_tag::UNKNOWN:
        case backend_tag::CUDA:
            break;  // dispatched through the virtual fallback below.
    }
    seg->backend->destroy(seg);  // virtual fallback: CUDA, UNKNOWN, any unlisted backend.
}

#endif

}  // namespace tr::mem
