/**
 * @file
 * @brief mem_cuda implementation — CUDA *runtime* API only (no kernels), so a host C++ compiler +
 *        libcudart builds it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Built only with LIBTRACER_WITH_CUDA.
 */

#include "libtracer/mem_cuda.hpp"

#include <cuda_runtime.h>

#include <new>

namespace tr::mem {
namespace {

class cuda_backend_t final : public mem_backend_t {
   public:
    cuda_backend_t() noexcept : mem_backend_t("mem_cuda") {}

    view::segment_t* alloc(std::size_t size, alloc_hint_t /*hint*/) override {
        void* dptr = nullptr;
        if (size != 0 && cudaMalloc(&dptr, size) != cudaSuccess) return nullptr;
        auto* seg = new (std::nothrow)
            view::segment_t(this, std::span<std::byte>(static_cast<std::byte*>(dptr), size));
        if (seg == nullptr) {
            if (dptr != nullptr) cudaFree(dptr);
            return nullptr;
        }
        return seg;
    }

    void destroy(view::segment_t* seg) noexcept override {
        if (seg->bytes.data() != nullptr) cudaFree(seg->bytes.data());
        delete seg;
    }

    /** @brief Device memory is not host-cached; the cache hooks reduce to a stream barrier. */
    void after_io(view::segment_t* /*seg*/, io_dir_t /*dir*/) noexcept override {
        (void)cudaDeviceSynchronize();
    }

    [[nodiscard]] mem_space_t space() const noexcept override { return mem_space_t::DEVICE; }
    /**
     * @brief TU-local (LIBTRACER_WITH_CUDA only): tagged CUDA, but not in backend_set.cpp's fast
     *        switch, so destroy_dispatch routes it through the virtual `destroy` above and
     *        mem::transfer routes it through cuda_transfer (below).
     */
    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::CUDA; }

    /**
     * @brief Module-set traits (ADR-0047 §2).
     *
     * Device memory is not host-cached; the copy
     * and its stream barrier live in cuda_transfer, so `needs_cache_ops` is not
     * read on the CUDA path — set true for an honest, non-trivial cache contract.
     */
    static constexpr bool needs_cache_ops =
        true; /**< @brief The cache hooks reduce to a CUDA stream barrier (after_io). */
    static constexpr bool is_isr_safe =
        false; /**< @brief `cudaMalloc`/`cudaFree` are not ISR-safe. */
    static constexpr bool owns_bytes =
        true; /**< @brief Owns the `cudaMalloc`'d device allocation. */
};

}  // namespace

mem_backend_t& cuda_backend() noexcept {
    static cuda_backend_t backend;
    return backend;
}

bool cuda_transfer(view::segment_t* seg, std::span<std::byte> host, io_dir_t dir) noexcept {
    if (seg == nullptr || host.size() > seg->bytes.size()) return false;
    seg->backend->before_io(seg, dir);  // no-op default (device memory is not host-cached).
    const cudaError_t rc =
        (dir == io_dir_t::CPU_TO_DEVICE)
            ? cudaMemcpy(seg->bytes.data(), host.data(), host.size(), cudaMemcpyHostToDevice)
            : cudaMemcpy(host.data(), seg->bytes.data(), host.size(), cudaMemcpyDeviceToHost);
    seg->backend->after_io(seg, dir);  // cudaDeviceSynchronize — after_io's first caller.
    return rc == cudaSuccess;
}

}  // namespace tr::mem

namespace tr::view {

segment_ptr_t cuda_alloc(std::size_t size) {
    return segment_ptr_t::adopt(mem::cuda_backend().alloc(size, mem::alloc_hint_t::NONE));
}

}  // namespace tr::view
