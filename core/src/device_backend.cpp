/**
 * @file
 * @brief The `DEVICE`-space backend registry — `tr::mem::transfer`'s out-of-core arm (#1381).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A vendor accelerator backend lives in the `backends/` tier, not in core (docs/adr/0024
 * Amendment 1). It plugs in by registering the pair `{its mem_backend_t, its device
 * byte-move}` here, exactly as an out-of-tree transport registers `{kind, factory}` with
 * `transport_vertex_t::register_transport_type` (docs/adr/0043 §1) — core holds the seam
 * and never names the module, so the dependency arrow points up the tiers only.
 *
 * A TU of its own, and that is the point: `backend_set.cpp`'s single-backend
 * (LIBTRACER_BACKEND_SET_POOL_ONLY) fold has no device arm to dispatch, references nothing
 * here, and so an MCU link never pulls this table in — 0 B rather than "a small table".
 */
#include <atomic>
#include <cstddef>
#include <span>

#include "libtracer/backend.hpp"
#include "libtracer/config.hpp"
#include "libtracer/segment.hpp"

namespace tr::mem {

namespace {

/** @brief One registration: the backend that owns the segments, and the hook that moves them. */
struct device_slot_t {
    std::atomic<const mem_backend_t*> backend{nullptr}; /**< @brief Claimed slot's key; the
                                                           publishing store (release). */
    std::atomic<device_transfer_fn_t> fn{nullptr};      /**< @brief The registered byte-move. */
};

/**
 * @brief The bounded table: `kDeviceBackendSlots` entries, constant-initialized, so it is
 *        `.bss` (never flash-backed `.data`) and costs nothing to start.
 */
constinit device_slot_t g_slots[kDeviceBackendSlots];

}  // namespace

bool register_device_backend(const mem_backend_t& backend, device_transfer_fn_t fn) noexcept {
    if (fn == nullptr) return false;
    // Replace-in-place first, so re-registering the same backend can never consume a second
    // slot (`insert_or_assign` semantics — the register_transport_type contract).
    for (device_slot_t& slot : g_slots) {
        if (slot.backend.load(std::memory_order_relaxed) == &backend) {
            slot.fn.store(fn, std::memory_order_release);
            return true;
        }
    }
    for (device_slot_t& slot : g_slots) {
        if (slot.backend.load(std::memory_order_relaxed) == nullptr) {
            // The hook is published BEFORE the key a lookup matches on: a reader that sees
            // the backend is guaranteed to see the hook that goes with it.
            slot.fn.store(fn, std::memory_order_relaxed);
            slot.backend.store(&backend, std::memory_order_release);
            return true;
        }
    }
    return false;  // table full — registers nothing (config.hpp: kDeviceBackendSlots).
}

namespace detail {

bool device_transfer(view::segment_t* seg, std::span<std::byte> host, io_dir_t dir) noexcept {
    for (const device_slot_t& slot : g_slots) {
        if (slot.backend.load(std::memory_order_acquire) == seg->backend) {
            return slot.fn.load(std::memory_order_relaxed)(seg, host, dir);
        }
    }
    // Nothing registered for this backend: the same refusal every unrecognized DEVICE segment
    // got before the registry existed (#928) — a device pointer is not CPU-dereferenceable.
    return false;
}

}  // namespace detail

}  // namespace tr::mem
