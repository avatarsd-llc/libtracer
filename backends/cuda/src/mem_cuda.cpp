/**
 * @file
 * @brief mem_cuda implementation — CUDA *runtime* API only (no kernels), so a host C++ compiler +
 *        libcudart builds it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The backends/cuda tier module (#1381, docs/adr/0024 Amendment 1) — built by
 * backends/cuda/CMakeLists.txt, never by core.
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
    // No `tag()` override, and none is possible: `backend_tag` is closed over the backends
    // core itself compiles (#1381). This backend leaves the default UNKNOWN, so
    // destroy_dispatch takes the virtual `destroy` above — the SAME arm the CUDA enumerator
    // routed to, since it was never in the fast switch. `mem::transfer` finds cuda_transfer
    // by the backend's IDENTITY instead, through the registration below.

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
    static constexpr bool is_nonblocking =
        false; /**< @brief `cudaMalloc`/`cudaFree` may block in the driver (#928). */
    static constexpr bool owns_bytes =
        true; /**< @brief Owns the `cudaMalloc`'d device allocation. */
};

/** @brief The one backend object, constructed on first use. Its ADDRESS is the registry key. */
cuda_backend_t& instance() noexcept {
    static cuda_backend_t backend;
    return backend;
}

}  // namespace

bool register_cuda_backend() noexcept {
    return register_device_backend(instance(), &cuda_transfer);
}

mem_backend_t& cuda_backend() noexcept {
    cuda_backend_t& backend = instance();
    // Registering on first use is what keeps the backend and its transfer hook from ever
    // disagreeing: every route to a device segment runs through cuda_alloc -> cuda_backend(),
    // so by the time such a segment exists the hook is in core's table and mem::transfer can
    // find it. A composition root that wants the registration EARLIER — or wants to SEE it
    // refuse a full table — calls register_cuda_backend() itself; it is idempotent.
    //
    // `instance()` is fully initialized before this line, so the call below cannot re-enter
    // this static's own guard.
    [[maybe_unused]] static const bool registered = register_cuda_backend();
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
