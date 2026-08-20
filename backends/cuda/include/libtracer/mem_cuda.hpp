/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_cuda — an L0 backend over CUDA device memory (docs/adr/0024). Its segments
 * are DEVICE-space: the bytes are a CUDA device pointer the CPU MUST NOT
 * dereference. A mem_cuda segment therefore backs only an opaque VALUE payload;
 * the TLV header/trailer stay in a HOST segment, forming a heterogeneous
 * host+device rope.
 *
 * A TIER MODULE, not part of core (docs/adr/0024 Amendment 1, #1381): it lives under
 * backends/cuda/ with its own CMake project and plugs into core through
 * tr::mem::register_device_backend, the way an out-of-tree transport plugs in through
 * register_transport_type. Nothing in core/ names CUDA. Link libtracer_cuda to get it;
 * the .cpp uses the CUDA *runtime* API (no kernels, so a host compiler + libcudart
 * suffices — no nvcc).
 */
#pragma once

#include <cstddef>
#include <span>

#include "libtracer/backend.hpp"
#include "libtracer/segment.hpp"

/**
 * @file
 * @brief The `mem_cuda` L0 GPU backend (`tr::mem`) + its L1 device helpers (`tr::view`).
 */

namespace tr::mem {

/**
 * @brief The process-wide CUDA device backend (`cudaMalloc`/`cudaFree`; `space()` == DEVICE).
 *
 * Constructing it also **registers** it (see @ref register_cuda_backend), so a caller that
 * only ever allocates — `tr::view::cuda_alloc` — can never meet a `tr::mem::transfer` that
 * does not know where to route its segments.
 */
[[nodiscard]] mem_backend_t& cuda_backend() noexcept;

/**
 * @brief Register @ref cuda_backend with core's device-backend registry, so
 *        `tr::mem::transfer` routes its `DEVICE` segments to @ref cuda_transfer.
 *
 * The `tr::net::quic_transport_factory` of this tier: the module supplies the value, the
 * composition root (or, here, @ref cuda_backend's own first construction) wires it in, and
 * core never names CUDA. Idempotent — calling it twice replaces the same slot's hook.
 * @retval false Core's bounded table is full (`tr::mem::kDeviceBackendSlots`).
 */
[[nodiscard]] bool register_cuda_backend() noexcept;

/**
 * @brief The device byte-move behind `tr::mem::transfer` for a CUDA (`DEVICE`) segment:
 *        `cudaMemcpy` in direction @p dir, bracketed by the backend's cache hooks
 *        (`after_io` == the CUDA stream barrier).
 *
 * Declared here but **defined in mem_cuda.cpp** so `cudaMemcpy` stays TU-local. It is what
 * @ref register_cuda_backend hands core. Not called directly — use `tr::mem::transfer`,
 * which routes this backend's segments here.
 */
[[nodiscard]] bool cuda_transfer(view::segment_t* seg, std::span<std::byte> host,
                                 io_dir_t dir) noexcept;

}  // namespace tr::mem

namespace tr::view {

/**
 * @brief Allocate a CUDA device segment of @p size bytes (DEVICE space).
 *
 * An L1 handle producer (docs/adr/0016 §2). The bytes live in GPU memory; the
 * resulting view reports @ref view_t::is_device and must not be CPU-dereferenced.
 * @retval {} An empty handle if `cudaMalloc` fails.
 */
[[nodiscard]] segment_ptr_t cuda_alloc(std::size_t size);

}  // namespace tr::view
