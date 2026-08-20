/**
 * @file
 * @brief The build-time-closed module-set destroy dispatch (ADR-0047 §2).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This is the
 * one TU that includes every fast-set backend's concrete type, so
 * segment_ptr_t::reset (segment.hpp) can reclaim a segment by its backend_tag —
 * a switch to a devirtualized direct call — while keeping the L0 seam
 * (segment.hpp / backend.hpp) free of any dependency on the concrete backends.
 *
 * Every fast case dispatches through the backend's `final`-class type with a
 * qualified call (a non-virtual, inlinable direct call). Any other tag (UNKNOWN, or a
 * backend not linked into the fast set — every out-of-core device backend is) falls
 * through to the backend's virtual `destroy`, so the result is identical to the
 * pre-ADR-0047 `seg->backend->destroy(seg)` for every backend — the tag is a fast
 * path, never a correctness dependency.
 *
 * The one arm this TU does NOT own is the DEVICE-space byte-move: a vendor accelerator
 * backend lives in the `backends/` tier and registers its hook through
 * `register_device_backend` (`device_backend.cpp`, #1381), so no vendor name appears
 * here and adding a second accelerator changes nothing in core.
 */
#include <cstring>
#include <span>

#include "libtracer/backend.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/segment.hpp"

#ifndef LIBTRACER_BACKEND_SET_POOL_ONLY
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_heap.hpp"
#endif

namespace tr::mem {

namespace {

// The host byte-move behind mem::transfer: a memcpy in direction `dir`, bracketed
// by the backend's cache hooks only when its `needs_cache_ops` trait is set — so a
// cacheless backend (every one in-tree) folds the hooks away at compile time. This
// is the module-set traits' first in-tree consumer (ADR-0047 §2, review finding #8).
template <class Backend>
bool transfer_host(view::segment_t* seg, std::span<std::byte> host, io_dir_t dir) noexcept {
    if (host.size() > seg->bytes.size()) return false;
    if constexpr (Backend::needs_cache_ops) seg->backend->before_io(seg, dir);
    if (dir == io_dir_t::CPU_TO_DEVICE) {
        std::memcpy(seg->bytes.data(), host.data(), host.size());
    } else {
        std::memcpy(host.data(), seg->bytes.data(), host.size());
    }
    if constexpr (Backend::needs_cache_ops) seg->backend->after_io(seg, dir);
    return true;
}

// Fallback for a backend not in the fast set (a user backend tagged UNKNOWN): the
// same memcpy, but bracketed by the virtual cache hooks unconditionally (its traits
// are not statically known here). Its DEVICE-space refusal is now redundant with the
// hoisted guard in `transfer` (#928) and kept as defence in depth.
bool transfer_generic(view::segment_t* seg, std::span<std::byte> host, io_dir_t dir) noexcept {
    if (host.size() > seg->bytes.size() || seg->space == mem_space_t::DEVICE) return false;
    seg->backend->before_io(seg, dir);
    if (dir == io_dir_t::CPU_TO_DEVICE) {
        std::memcpy(seg->bytes.data(), host.data(), host.size());
    } else {
        std::memcpy(host.data(), seg->bytes.data(), host.size());
    }
    seg->backend->after_io(seg, dir);
    return true;
}

}  // namespace

#ifdef LIBTRACER_BACKEND_SET_POOL_ONLY

// Single-member module set (the MCU profile): the target links only mem_pool, so
// the dispatch folds to one predicted direct call — no switch, no other backends'
// destroy code pulled in. This is the ADR-0047 §2 fold: on a single-backend target
// the seam compiles to nothing more than the tag compare and the direct release.
// `-D` this per target.
//
// The tag compare is NOT optional here, exactly as in the `transfer` sibling below
// (#922). A POOL_ONLY target still sees segments whose backend is not a `pool_t`:
// `synchronized_pool_t` re-points every segment it hands out to ITSELF with an
// UNKNOWN tag (mem_pool.hpp) precisely so reclaim takes the locked virtual
// `destroy` — and it is a `mem_backend_t` holding a `pool_t` MEMBER, not a
// `pool_t`, so reinterpreting it reads `slab_`/`stride_` from the wrong offsets
// and bypasses the critical section. Any `tr::view::borrow()`ed segment is the
// same story. The tag stays a fast path, never a correctness dependency.
void destroy_dispatch(view::segment_t* seg) noexcept {
    if (seg->btag == backend_tag::POOL) {
        static_cast<pool_t*>(seg->backend)->pool_t::destroy(seg);
        return;
    }
    seg->backend->destroy(seg);  // virtual fallback: synchronized/borrowed/any other backend.
}

// Single-backend set: only mem_pool is linked, so the transfer dispatch folds too.
// The DEVICE-space guard is hoisted here exactly as in the multi-member variant (#928),
// and it is UNCONDITIONAL: a single-backend target has no device arm at all, so it never
// consults the device-backend registry and never links `device_backend.cpp` (#1381).
bool transfer(view::segment_t* seg, std::span<std::byte> host, io_dir_t dir) noexcept {
    if (seg == nullptr || seg->space == mem_space_t::DEVICE) return false;
    if (seg->btag == backend_tag::POOL) return transfer_host<pool_t>(seg, host, dir);
    return transfer_generic(seg, host, dir);
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
            break;  // dispatched through the virtual fallback below.
    }
    seg->backend->destroy(seg);  // virtual fallback: UNKNOWN, any unlisted backend.
}

// The multi-member host set: a switch to the per-backend host memcpy (with the
// backend's `needs_cache_ops` trait gating its cache hooks), a DEVICE-space segment
// routed through the registry to its own backend's hook, and the generic memcpy for
// anything else.
bool transfer(view::segment_t* seg, std::span<std::byte> host, io_dir_t dir) noexcept {
    if (seg == nullptr) return false;
    // Hoisted DEVICE-space guard (#928): a device pointer is not CPU-dereferenceable, so no
    // host-memcpy arm may ever see one. Before this guard the backend TAG decided the outcome:
    // a BORROWED_DEVICE segment reached `transfer_host`'s memcpy while a semantically identical
    // UNKNOWN-tagged device segment got `transfer_generic`'s clean false — and the tag is
    // documented as a fast path with identical results, never a correctness dependency.
    //
    // The one way past it is the backend's OWN registered device copy (#1381): the registry
    // answers `false` for a backend nobody registered, which is exactly what the pre-registry
    // guard did for every DEVICE segment that was not the one vendor tag core used to name.
    if (seg->space == mem_space_t::DEVICE) return detail::device_transfer(seg, host, dir);
    switch (seg->btag) {
        case backend_tag::HEAP:
            return transfer_host<heap_backend_t>(seg, host, dir);
        case backend_tag::POOL:
            return transfer_host<pool_t>(seg, host, dir);
        case backend_tag::BORROWED:
            return transfer_host<borrowed_backend_t>(seg, host, dir);
        case backend_tag::BORROWED_DEVICE:
            return transfer_host<borrowed_device_backend_t>(seg, host, dir);
        case backend_tag::UNKNOWN:
            break;
    }
    return transfer_generic(seg, host, dir);
}

#endif

}  // namespace tr::mem
