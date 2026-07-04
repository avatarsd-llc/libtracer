/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_cuda — an L0 backend over CUDA device memory (docs/adr/0024). Its segments
 * are DEVICE-space: the bytes are a CUDA device pointer the CPU MUST NOT
 * dereference. A mem_cuda segment therefore backs only an opaque VALUE payload;
 * the TLV header/trailer stay in a HOST segment, forming a heterogeneous
 * host+device rope. Built only when LIBTRACER_WITH_CUDA is set (needs the CUDA
 * toolkit); the .cpp uses the CUDA *runtime* API (no kernels, so a host compiler
 * + libcudart suffices — no nvcc).
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

/** @brief The process-wide CUDA device backend (`cudaMalloc`/`cudaFree`; `space()` == DEVICE). */
[[nodiscard]] mem_backend_t& cuda_backend() noexcept;

/**
 * @brief The device byte-move behind @ref transfer for a CUDA (`DEVICE`) segment:
 *        `cudaMemcpy` in direction @p dir, bracketed by the backend's cache hooks
 *        (`after_io` == the CUDA stream barrier).
 *
 * Declared here but **defined in mem_cuda.cpp** so `cudaMemcpy` stays TU-local;
 * `backend_set.cpp`'s @ref transfer calls it only under `LIBTRACER_WITH_CUDA`.
 * Not called directly — use @ref transfer, which routes CUDA-tagged segments here.
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
