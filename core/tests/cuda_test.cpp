/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_cuda GPU test (docs/adr/0024). Built only with LIBTRACER_WITH_CUDA; run on
 * a real GPU (locally, e.g. tools/test-cuda.sh — never in CI, which has no GPU).
 */
#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_cuda.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

namespace {
int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}
}  // namespace

int main() {
    std::printf("mem_cuda — GPU device backend (docs/adr/0024):\n");

    // 1. allocate device memory
    tr::view::segment_ptr_t dev = tr::view::cuda_alloc(64);
    check(static_cast<bool>(dev), "cuda_alloc(64) succeeds on the GPU");
    if (!dev) {
        std::printf("FAILURES (no usable CUDA device)\n");
        return 1;
    }
    check(dev->space == tr::mem::mem_space_t::DEVICE, "cuda segment.space == DEVICE");

    // 2. round-trip host -> device -> host (proves real GPU memory works)
    std::array<std::byte, 64> src{};
    for (std::size_t i = 0; i < src.size(); ++i) src[i] = std::byte(i + 1);
    check(tr::mem::transfer(dev.get(), src, tr::mem::io_dir_t::CPU_TO_DEVICE),
          "host -> device transfer ok");
    std::array<std::byte, 64> dst{};
    check(tr::mem::transfer(dev.get(), dst, tr::mem::io_dir_t::DEVICE_TO_CPU),
          "device -> host transfer ok");
    bool same = true;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] != dst[i]) same = false;
    }
    check(same, "device round-trip preserves the bytes");

    // 3. heterogeneous host(header) + device(payload) rope — the mem_cuda TLV shape
    std::array<std::byte, 4> header{std::byte{0x01}, std::byte{0x40}, std::byte{0x40},
                                    std::byte{0x00}};
    tr::view::rope_t rope(tr::view::view_t::over(tr::view::borrow(header)));
    rope.append(tr::view::view_t::over(dev));  // moves the device segment into the rope
    check(!rope.all_host(), "host+device rope is heterogeneous (not all_host)");
    check(rope.flatten().empty(), "flatten() refuses the heterogeneous rope (no device deref)");
    check(rope.total_length() == 4 + 64, "rope spans header(4) + device payload(64)");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
