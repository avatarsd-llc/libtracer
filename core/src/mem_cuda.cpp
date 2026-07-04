/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_cuda implementation — CUDA *runtime* API only (no kernels), so a host C++
 * compiler + libcudart builds it. Built only with LIBTRACER_WITH_CUDA.
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

    // Device memory is not host-cached; the cache hooks reduce to a stream barrier.
    void after_io(view::segment_t* /*seg*/, io_dir_t /*dir*/) noexcept override {
        (void)cudaDeviceSynchronize();
    }

    [[nodiscard]] mem_space_t space() const noexcept override { return mem_space_t::DEVICE; }
    // TU-local (LIBTRACER_WITH_CUDA only): tagged CUDA, but not in backend_set.cpp's
    // fast switch, so destroy_dispatch routes it through the virtual `destroy` above.
    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::CUDA; }
};

}  // namespace

mem_backend_t& cuda_backend() noexcept {
    static cuda_backend_t backend;
    return backend;
}

}  // namespace tr::mem

namespace tr::view {

segment_ptr_t cuda_alloc(std::size_t size) {
    return segment_ptr_t::adopt(mem::cuda_backend().alloc(size, mem::alloc_hint_t::NONE));
}

bool cuda_copy_from_host(const segment_ptr_t& dev, std::span<const std::byte> host) {
    if (!dev || host.size() > dev->bytes.size()) return false;
    return cudaMemcpy(dev->bytes.data(), host.data(), host.size(), cudaMemcpyHostToDevice) ==
           cudaSuccess;
}

bool cuda_copy_to_host(const segment_ptr_t& dev, std::span<std::byte> host) {
    if (!dev || host.size() > dev->bytes.size()) return false;
    return cudaMemcpy(host.data(), dev->bytes.data(), host.size(), cudaMemcpyDeviceToHost) ==
           cudaSuccess;
}

}  // namespace tr::view
